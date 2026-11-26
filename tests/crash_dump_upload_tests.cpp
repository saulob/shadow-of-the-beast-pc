// The Bug Report page's crash dump: offered for a crashed session, optional, announced with the report, uploaded in chunks
// with the report's receipt and resumed from what the service holds, never filing a second report. A stand-in service in
// this process answers; nothing reaches the network.
#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include "crash_report_test_support.h"
#include "crypto.h"
#include "http_post.h"
#include "incident_client.h"
#include "json_lite.h"

using namespace crash_test;
using namespace Sotb;

namespace {

std::string Hex(const uint8_t* data, size_t size) {
    std::string hex;
    for (size_t i = 0; i < size; ++i) {
        hex += "0123456789abcdef"[data[i] >> 4];
        hex += "0123456789abcdef"[data[i] & 15];
    }
    return hex;
}

std::string Sha256Hex(const std::string& bytes) {
    uint8_t digest[32];
    crypto::Sha256(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), digest);
    return Hex(digest, 32);
}

std::string FromBase64(std::string_view text, bool& valid) {
    static const std::string kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    valid = text.size() % 4 == 0;
    for (size_t at = 0; valid && at < text.size(); at += 4) {
        uint32_t word = 0;
        int pad = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text[at + i];
            if (c == '=' && at + 4 == text.size() && i >= 2) {
                ++pad;
                word <<= 6;
                continue;
            }
            const size_t value = kAlphabet.find(c);
            if (value == std::string::npos || pad) valid = false;
            word = word << 6 | uint32_t(value & 63);
        }
        out += char(word >> 16);
        if (pad < 2) out += char(word >> 8 & 255);
        if (pad < 1) out += char(word & 255);
    }
    return out;
}

std::string Progress(uint64_t stored, uint64_t bytes, bool complete) {
    return "{\"stored\":" + std::to_string(stored) + ",\"bytes\":" + std::to_string(bytes) + ",\"complete\":" + (complete ? "true" : "false") + "}";
}

// The service's rules (server/shadow-bug-reports/src/service.js) for one report and its dump, plus ways to fail.
struct StandIn {
    struct Filed {
        std::string ticket, receipt, submission;
        bool dump = false;
        uint64_t bytes = 0;
        std::string sha256, stored;
        bool complete = false;
    };
    std::vector<Filed> reports;
    std::vector<std::pair<std::string, std::string>> calls;
    std::set<size_t> lose_answer;  // calls answered by the service whose answer never arrives
    std::set<size_t> unreachable;  // calls that never arrive
    int chunk_status = 0;          // answers every chunk with this status instead
    size_t grow_after = SIZE_MAX;  // after this call, the first report's dump gains `grow` from another writer
    std::string grow;
    uint64_t received_bytes = 0;

    PostJson Transport() {
        return [this](const std::string& url, const std::string& body, const HttpLimits& limits) {
            const size_t index = calls.size();
            calls.emplace_back(url.substr(url.find("/v1/")), body);
            last_limits = limits;
            if (unreachable.count(index)) return HttpReply{HttpFailure::kUnreachable, 0, "", 0};
            HttpReply reply = Handle(calls.back().first, body);
            if (index == grow_after && !reports.empty()) reports[0].stored += grow;
            if (lose_answer.count(index)) return HttpReply{HttpFailure::kTimedOut, 0, "", 0};
            return reply;
        };
    }

    Filed* Claimed(const json::Value& body) {
        for (Filed& filed : reports) {
            if (filed.ticket == body.StringOr("ticket") && filed.receipt == body.StringOr("receipt")) return &filed;
        }
        return nullptr;
    }

    static std::string Answer(const Filed& filed) {
        return "{\"ticket\":\"" + filed.ticket + "\",\"receipt\":\"" + filed.receipt + "\",\"submitted_at\":1700000000000" +
               (filed.dump ? ",\"dump\":\"expected\",\"dump_chunk_bytes\":1048576" : "") + "}";
    }

    HttpReply Handle(const std::string& path, const std::string& text) {
        const auto body = json::Parse(text);
        if (!body || !body->IsObject()) return {HttpFailure::kNone, 400, "{\"error\":\"bad_request\"}", 0};
        if (path == "/v1/reports") {
            const std::string submission = body->StringOr("submission");
            for (const Filed& filed : reports) {
                if (!submission.empty() && filed.submission == submission) return {HttpFailure::kNone, 200, Answer(filed), 0};
            }
            Filed filed;
            filed.ticket = std::string("SOB-AAAAA") + "ABCDEFGHJK"[reports.size() % 10];
            filed.receipt = std::string(64, "abcdef"[reports.size() % 6]);
            filed.submission = submission;
            filed.dump = body->Find("dump_file") != nullptr;
            filed.bytes = uint64_t(body->NumberOr("dump_bytes"));
            filed.sha256 = body->StringOr("dump_sha256");
            reports.push_back(filed);
            return {HttpFailure::kNone, 201, Answer(reports.back()), 0};
        }
        Filed* filed = Claimed(*body);
        if (!filed) return {HttpFailure::kNone, 404, "{\"error\":\"not_found\"}", 0};
        if (!filed->dump) return {HttpFailure::kNone, 400, "{\"error\":\"bad_request\"}", 0};
        if (path == "/v1/reports/dump") return {HttpFailure::kNone, 200, Progress(filed->stored.size(), filed->bytes, filed->complete), 0};
        if (chunk_status) return {HttpFailure::kNone, chunk_status, "{\"error\":\"unavailable\"}", 0};
        bool valid = false;
        const std::string bytes = FromBase64(body->StringOr("data"), valid);
        const uint64_t offset = uint64_t(body->NumberOr("offset"));
        if (!valid || bytes.empty() || bytes.size() > kDumpChunkBytes) return {HttpFailure::kNone, 400, "{\"error\":\"bad_request\"}", 0};
        if (filed->complete || offset != filed->stored.size()) {
            return {HttpFailure::kNone, 409, "{\"error\":\"offset\"," + Progress(filed->stored.size(), filed->bytes, filed->complete).substr(1), 0};
        }
        if (offset + bytes.size() > filed->bytes) return {HttpFailure::kNone, 400, "{\"error\":\"bad_request\"}", 0};
        filed->stored += bytes;
        received_bytes += bytes.size();
        if (filed->stored.size() == filed->bytes) {
            if (!filed->stored.starts_with("MDMP") || Sha256Hex(filed->stored) != filed->sha256) {
                filed->dump = false;
                filed->stored.clear();
                return {HttpFailure::kNone, 400, "{\"error\":\"bad_request\",\"detail\":\"dump_sha256\"}", 0};
            }
            filed->complete = true;
        }
        return {HttpFailure::kNone, 200, Progress(filed->stored.size(), filed->bytes, filed->complete), 0};
    }

    size_t Count(std::string_view path) const {
        return size_t(std::count_if(calls.begin(), calls.end(), [&](const auto& call) { return call.first == path; }));
    }

    HttpLimits last_limits;
};

ServiceConfig Configured(StandIn& service, std::vector<std::string>* lines = nullptr) {
    ServiceConfig config;
    config.endpoint = ResolveEndpoint("https://reports.example", "");
    config.post = service.Transport();
    if (lines) config.log = [lines](const std::string& line) { lines->push_back(line); };
    return config;
}

struct Crash {
    GameLog log;
    std::string bytes;
};

Crash CrashedSession(const fs::path& folder, int number, size_t dump_bytes) {
    MakeLog(folder, number, kGameplayHead + CrashTail(number));
    std::string bytes = DumpBytes(dump_bytes, unsigned(number));
    MakeDump(folder, number, bytes);
    return {Found(folder / LogName(number)), std::move(bytes)};
}

IncidentPackage PackageOf(const GameLog& log, bool attach) {
    return *BuildIncidentPackage("The game crashed when the boss appeared", log, Facts(), Private(), attach);
}

// 1, 2, 3: the page's model of a crashed session and what it offers.
void TestOffered(const fs::path& scratch) {
    const Crash crash = CrashedSession(scratch / "offered", 10, 4096);
    CHECK(crash.log.Crashed() && SessionEndText(crash.log) == std::string("Crash detected"));
    CHECK(crash.log.CanAttachDump() && DefaultAttachCrashDump(crash.log) && crash.log.dump->name == DumpName(10));

    const fs::path folder = scratch / "refused";
    MakeLog(folder, 11, kGameplayHead + CrashTail(11));
    MakeDump(folder, 11, DumpBytes(kMostDumpBytes + 1));
    const GameLog big = Found(folder / LogName(11));
    CHECK(big.Crashed() && big.dump && !big.CanAttachDump() && !DefaultAttachCrashDump(big) && big.dump_problem == DumpProblem::kTooLarge);
    const IncidentPackage refused = PackageOf(big, true);
    CHECK(!refused.crash_dump && refused.crash_dump_problem == DumpProblem::kTooLarge);
    CHECK(!Has(IncidentRequestJson(refused), "dump_"));
    CHECK(Has(IncidentPreviewText(refused), "Crash dump: not attached. The crash dump is larger than 8 MB."));
}

// 4, 5, 11: the box decides, and the request and preview say what goes.
void TestOptional(const fs::path& scratch) {
    const Crash crash = CrashedSession(scratch / "optional", 20, 5000);
    const IncidentPackage on = PackageOf(crash.log, true);
    CHECK(on.crash_dump && on.crash_dump_requested && on.crash_dump->bytes == 5000 && on.crash_dump->sha256 == Sha256Hex(crash.bytes));
    const auto request = json::Parse(IncidentRequestJson(on));
    CHECK(request && request->StringOr("dump_file") == DumpName(20) && request->NumberOr("dump_bytes") == 5000);
    CHECK(request && request->StringOr("dump_sha256") == Sha256Hex(crash.bytes) && request->StringOr("session_end") == "crash");
    CHECK(request && request->StringOr("session_stage") == "first frame presented");
    CHECK(Has(IncidentPreviewText(on), "Crash dump: " + DumpName(20) + " (5 KB, sent as a file after the report, not shown here)"));
    CHECK(!Has(IncidentPreviewText(on), "MDMP"));

    const IncidentPackage off = PackageOf(crash.log, false);
    CHECK(!off.crash_dump && !off.crash_dump_requested);
    CHECK(!Has(IncidentRequestJson(off), "dump_") && !Has(IncidentPreviewText(off), "Crash dump"));
    CHECK(off.Value("session_end") == "crash" && Has(off.Value("session_detail"), "FATAL exception"));
}

// 6, 7: a report without a dump is one request; with one, the dump follows under the same Report ID.
void TestSend(const fs::path& scratch) {
    const Crash crash = CrashedSession(scratch / "send", 30, 2 * kDumpChunkBytes + 12345);
    StandIn without;
    const ReportReceipt plain = FileReport(Configured(without), PackageOf(crash.log, false));
    CHECK(plain.outcome == ReportOutcome::kSent && !plain.dump_expected && plain.dump == DumpUpload::kNotRequested);
    CHECK(without.calls.size() == 1 && without.reports.size() == 1);

    StandIn service;
    std::vector<std::string> lines;
    const ServiceConfig config = Configured(service, &lines);
    const IncidentPackage package = PackageOf(crash.log, true);
    const ReportReceipt filed = FileReport(config, package);
    CHECK(filed.outcome == ReportOutcome::kSent && filed.dump_expected);
    CHECK(UploadCrashDump(config, filed.ticket, filed.credential, *package.crash_dump) == DumpUpload::kUploaded);
    CHECK(service.reports.size() == 1 && service.reports[0].complete && service.reports[0].stored == crash.bytes);
    CHECK(service.received_bytes == crash.bytes.size());
    CHECK(service.Count("/v1/reports") == 1 && service.Count("/v1/reports/dump") == 1 && service.Count("/v1/reports/dump/chunk") == 3);
    CHECK(service.last_limits.send_ms >= 60000 && service.last_limits.total_ms >= 90000);
    for (size_t i = 1; i < service.calls.size(); ++i) {
        const auto body = json::Parse(service.calls[i].second);
        CHECK(body && body->StringOr("ticket") == filed.ticket && body->StringOr("receipt") == filed.credential);
    }
    for (const std::string& line : lines) CHECK(!Has(line, filed.credential) && !Has(line, scratch.string()) && !Has(line, "MDMP"));
    CHECK(std::any_of(lines.begin(), lines.end(), [&](const std::string& line) { return line == "Bug report: crash dump " + DumpName(30) + " uploaded for " + filed.ticket; }));
}

// The connection drops in the middle; the next try asks where the service stopped and sends only the rest.
void TestResume(const fs::path& scratch) {
    const Crash crash = CrashedSession(scratch / "resume", 40, 3 * kDumpChunkBytes + 7);
    const IncidentPackage package = PackageOf(crash.log, true);
    {
        StandIn service;
        const ReportReceipt filed = FileReport(Configured(service), package);
        service.unreachable = {3};
        CHECK(UploadCrashDump(Configured(service), filed.ticket, filed.credential, *package.crash_dump) == DumpUpload::kUploaded);
        CHECK(service.reports[0].stored == crash.bytes && service.received_bytes == crash.bytes.size());
        CHECK(service.Count("/v1/reports/dump") == 2 && service.Count("/v1/reports") == 1);
        const auto resumed = json::Parse(service.calls[5].second);
        CHECK(service.calls[4].first == "/v1/reports/dump" && resumed && resumed->NumberOr("offset") == int64_t(kDumpChunkBytes));
    }
    {
        // The service stored a chunk but its answer was lost: the repeat is refused with what is stored, and the upload goes on.
        StandIn service;
        const ReportReceipt filed = FileReport(Configured(service), package);
        service.lose_answer = {2};
        CHECK(UploadCrashDump(Configured(service), filed.ticket, filed.credential, *package.crash_dump) == DumpUpload::kUploaded);
        CHECK(service.reports[0].stored == crash.bytes && service.received_bytes == crash.bytes.size());
        CHECK(service.reports.size() == 1);
    }
    {
        // The service holds more than its last answer said: the chunk it already has is refused with where it ends, and
        // only what follows is sent.
        StandIn service;
        const ReportReceipt filed = FileReport(Configured(service), package);
        service.grow_after = 1;
        service.grow = crash.bytes.substr(0, kDumpChunkBytes);
        CHECK(UploadCrashDump(Configured(service), filed.ticket, filed.credential, *package.crash_dump) == DumpUpload::kUploaded);
        CHECK(service.reports[0].stored == crash.bytes && service.received_bytes == crash.bytes.size() - kDumpChunkBytes);
        CHECK(service.Count("/v1/reports/dump") == 1 && service.Count("/v1/reports/dump/chunk") == 4);
        const auto refused = json::Parse(service.calls[2].second);
        const auto next = json::Parse(service.calls[3].second);
        CHECK(refused && refused->NumberOr("offset") == 0 && next && next->NumberOr("offset") == int64_t(kDumpChunkBytes));
    }
    {
        StandIn service;
        const ReportReceipt filed = FileReport(Configured(service), package);
        service.reports[0].stored = crash.bytes;
        service.reports[0].complete = true;
        CHECK(UploadCrashDump(Configured(service), filed.ticket, filed.credential, *package.crash_dump) == DumpUpload::kUploaded);
        CHECK(service.Count("/v1/reports/dump/chunk") == 0);
    }
}

// 8, 10: a dump that does not arrive leaves the report filed; a retry uses its Report ID and files nothing.
void TestFailureAndRetry(const fs::path& scratch) {
    const Crash crash = CrashedSession(scratch / "failure", 50, kDumpChunkBytes + 99);
    const IncidentPackage package = PackageOf(crash.log, true);
    StandIn service;
    const ServiceConfig config = Configured(service);
    const ReportReceipt filed = FileReport(config, package);
    service.chunk_status = 500;
    CHECK(UploadCrashDump(config, filed.ticket, filed.credential, *package.crash_dump) == DumpUpload::kFailed);
    CHECK(service.reports.size() == 1 && !service.reports[0].complete && filed.outcome == ReportOutcome::kSent && IsTicket(filed.ticket));
    CHECK(service.Count("/v1/reports/dump") == size_t(kDumpUploadAttempts));

    service.chunk_status = 0;
    const size_t before = service.calls.size();
    CHECK(UploadCrashDump(config, filed.ticket, filed.credential, *package.crash_dump) == DumpUpload::kUploaded);
    CHECK(service.reports.size() == 1 && service.reports[0].complete && service.reports[0].stored == crash.bytes);
    for (size_t i = before; i < service.calls.size(); ++i) CHECK(service.calls[i].first != "/v1/reports");

    for (const int status : {400, 404, 413, 429}) {
        StandIn refusing;
        const ReportReceipt again = FileReport(Configured(refusing), package);
        refusing.chunk_status = status;
        CHECK(UploadCrashDump(Configured(refusing), again.ticket, again.credential, *package.crash_dump) == DumpUpload::kFailed);
        CHECK(refusing.Count("/v1/reports/dump") == 1 && refusing.Count("/v1/reports/dump/chunk") == 1);
    }

    // A dump that is no longer the one checked is not sent at all.
    StandIn changed;
    const ReportReceipt moved = FileReport(Configured(changed), package);
    Write(package.crash_dump->path, crash.bytes + "extra");
    CHECK(UploadCrashDump(Configured(changed), moved.ticket, moved.credential, *package.crash_dump) == DumpUpload::kFailed);
    CHECK(changed.Count("/v1/reports/dump") == 0 && changed.Count("/v1/reports/dump/chunk") == 0);
    Write(package.crash_dump->path, crash.bytes);

    CHECK(UploadCrashDump(config, "not-a-ticket", filed.credential, *package.crash_dump) == DumpUpload::kFailed);
    CHECK(UploadCrashDump(config, filed.ticket, "short", *package.crash_dump) == DumpUpload::kFailed);

    StandIn refused_dump;
    const IncidentPackage without = PackageOf(Found(MakeLog(scratch / "failure", 51, kGameplayHead + CrashTail(51))), true);
    const ReportReceipt sent = FileReport(Configured(refused_dump), without);
    CHECK(sent.outcome == ReportOutcome::kSent && sent.dump == DumpUpload::kNotAttached && !sent.dump_expected);
}

// 9: the same draft sent again, after an answer that never arrived, is the same report.
void TestNoDuplicate(const fs::path& scratch) {
    const Crash crash = CrashedSession(scratch / "duplicate", 60, 4096);
    const IncidentPackage package = PackageOf(crash.log, true);
    const std::string key = NewSubmissionKey();
    CHECK(key.size() == 64 && std::all_of(key.begin(), key.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
    CHECK(NewSubmissionKey() != key);

    StandIn service;
    const ServiceConfig config = Configured(service);
    service.lose_answer = {0};
    const ReportReceipt lost = FileReport(config, package, key);
    CHECK(lost.outcome == ReportOutcome::kTimedOut && lost.ticket.empty());
    const ReportReceipt again = FileReport(config, package, key);
    CHECK(again.outcome == ReportOutcome::kSent && service.reports.size() == 1 && again.ticket == service.reports[0].ticket);
    const ReportReceipt third = FileReport(config, package, key);
    CHECK(third.ticket == again.ticket && third.credential == again.credential && service.reports.size() == 1);
    CHECK(json::Parse(service.calls[0].second)->StringOr("submission") == key);
    CHECK(UploadCrashDump(config, again.ticket, again.credential, *package.crash_dump) == DumpUpload::kUploaded && service.reports[0].complete);
    FileReport(config, package, NewSubmissionKey());
    CHECK(service.reports.size() == 2);
    CHECK(!Has(IncidentRequestJson(package), "submission"));
}

// 12: no request carries a folder of this PC.
void TestNoPathsSent(const fs::path& scratch) {
    const fs::path folder = scratch / "ana.silva" / "Documents" / "logs";
    const Crash crash = CrashedSession(folder, 70, kDumpChunkBytes + 1);
    const IncidentPackage package = PackageOf(crash.log, true);
    StandIn service;
    const ServiceConfig config = Configured(service);
    const ReportReceipt filed = FileReport(config, package, NewSubmissionKey());
    UploadCrashDump(config, filed.ticket, filed.credential, *package.crash_dump);
    CHECK(service.calls.size() == 4);
    for (const auto& [path, body] : service.calls) {
        auto parsed = json::Parse(body);
        CHECK(parsed && parsed->IsObject());
        std::string metadata;
        for (const auto& member : parsed->object) {
            if (member.first != "data" && member.first != "log_text") metadata += member.first + "=" + member.second.string + ";";
        }
        CHECK(!Has(metadata, "ana.silva") && !Has(metadata, "Documents") && !Has(metadata, scratch.string()) && !Has(metadata, ":\\"));
        CHECK(!Has(metadata, "\\") && !Has(metadata, "/"));
    }
}

// The report and the chunks a real upload sends, for the service's own contract test (dumps.test.js).
void WriteContract(const fs::path& scratch) {
    const Crash crash = CrashedSession(scratch / "contract", 26, 2 * kDumpChunkBytes + 4321);
    const IncidentPackage package = PackageOf(crash.log, true);
    StandIn service;
    const ServiceConfig config = Configured(service);
    const ReportReceipt filed = FileReport(config, package, NewSubmissionKey());
    CHECK(UploadCrashDump(config, filed.ticket, filed.credential, *package.crash_dump) == DumpUpload::kUploaded);
    std::string chunks;
    for (const auto& [path, body] : service.calls) {
        if (path != "/v1/reports/dump/chunk") continue;
        const auto parsed = json::Parse(body);
        chunks += std::string(chunks.empty() ? "" : ",") + "{\"offset\":" + std::to_string(parsed->NumberOr("offset")) + ",\"data\":" +
                  json::Quote(parsed->StringOr("data")) + "}";
    }
    bool valid = false;
    std::string whole;
    for (const auto& [path, body] : service.calls) {
        if (path == "/v1/reports/dump/chunk") whole += FromBase64(json::Parse(body)->StringOr("data"), valid);
    }
    CHECK(whole == crash.bytes);
    const fs::path here = fs::current_path();
    Write(here / "bug-report-dump-request.json", service.calls[0].second);
    Write(here / "bug-report-dump-upload.json", "{\"dump_sha256\":\"" + Sha256Hex(crash.bytes) + "\",\"dump_bytes\":" +
                                                     std::to_string(crash.bytes.size()) + ",\"chunks\":[" + chunks + "]}");
    CHECK(fs::file_size(here / "bug-report-dump-upload.json") > crash.bytes.size());
}

// The page connects only from Send report and from Retry crash report upload, and a retry never files the report again.
void TestPageWiring() {
    const std::string page = Source("launcher/incident_page.cpp");
    const std::string client = Source("launcher/incident_client.cpp");
    CHECK(Count(page, "FileReport(") == 1 && Count(page, "UploadCrashDump(") == 2);
    CHECK(Has(page, "if (CanRetryDump(State(), sent_dump_.has_value(), !result_->credential.empty()) &&\n"
                    "            ImGui::Button(Tr(\"Retry crash report upload\"))) {\n"
                    "            RetryDump();"));
    CHECK(Count(page, "RetryDump();") == 1);
    const size_t retry = page.find("void IncidentPage::RetryDump()");
    const std::string retry_body = page.substr(retry, page.find("\n}\n", retry) - retry);
    CHECK(Has(retry_body, "UploadCrashDump(service, ticket, receipt, dump)") && !Has(retry_body, "FileReport("));
    CHECK(Has(page, "const bool sending = SendBusy(State());"));
    CHECK(Has(page, "if (submission_.empty()) submission_ = NewSubmissionKey();") && Has(page, "submission_.clear();"));
    CHECK(Has(page, "attach_crash_dump_ = selected_ < logs_.size() && DefaultAttachCrashDump(logs_[selected_]);"));
    CHECK(Has(page, "ImGui::BeginDisabled(sending || !attachable);") && Has(page, "attach_crash_dump_ && log.CanAttachDump()"));
    const size_t draw = page.find("void IncidentPage::DrawCrashDump(");
    const std::string draw_body = page.substr(draw, page.find("\n}\n", draw) - draw);
    CHECK(Has(draw_body, "log.dump->name") && !Has(draw_body, "dump->path") && !Has(draw_body, "Utf8("));
    const size_t upload = client.find("DumpStep SendDumpOnce(");
    const std::string upload_body = client.substr(upload, client.find("DumpUpload UploadCrashDump(") - upload);
    CHECK(!Has(upload_body, "kReportsPath") && Has(upload_body, "kDumpPath") && Has(upload_body, "kDumpChunkPath"));
    CHECK(!Has(client, "std::this_thread::sleep") && !Has(page, "std::this_thread::sleep"));

    // The launcher's bounds are the service's, so neither refuses what the other accepts.
    const std::string reports = Source("server/shadow-bug-reports/src/reports.js");
    CHECK(kMostDumpBytes == 8 * 1024 * 1024 && Has(reports, "dumpBytes: 8 * 1024 * 1024,"));
    CHECK(kFewestDumpBytes == 1024 && Has(reports, "dumpMinBytes: 1024,"));
    CHECK(kDumpChunkBytes == 1024 * 1024 && Has(reports, "dumpChunkBytes: 1024 * 1024,"));
    CHECK(kMostSessionDetailCharacters == 400 && Has(reports, "sessionDetailChars: 400,"));
    const std::string desk = Source("server/shadow-bug-reports/local/serve.js");
    CHECK(Has(desk, "dumpChunk: async") && Has(desk, "service.submit(report, address, dump, submission)"));
}

} // namespace

int main() {
    const fs::path scratch = Scratch("sotb-crash-dump-upload-tests");
    TestOffered(scratch);
    TestOptional(scratch);
    TestSend(scratch);
    TestResume(scratch);
    TestFailureAndRetry(scratch);
    TestNoDuplicate(scratch);
    TestNoPathsSent(scratch);
    WriteContract(scratch);
    TestPageWiring();
    return Finish("crash dump upload", scratch);
}
