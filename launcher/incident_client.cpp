#include "incident_client.h"

#include <algorithm>
#include <fstream>
#include <system_error>

#include "crypto.h"
#include "json_lite.h"

namespace Sotb {
namespace {
namespace fs = std::filesystem;

constexpr char kReportsPath[] = "/v1/reports";
constexpr char kStatusPath[] = "/v1/reports/status";
constexpr char kReadPath[] = "/v1/reports/read";
constexpr char kDumpPath[] = "/v1/reports/dump";
constexpr char kDumpChunkPath[] = "/v1/reports/dump/chunk";
constexpr char kFeedbackPath[] = "/v1/reports/feedback";

void Say(const ServiceConfig& config, const std::string& line) {
    if (config.log) config.log(line);
}

std::string TrimmedAscii(std::string_view text) {
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && blank(text.front())) text.remove_prefix(1);
    while (!text.empty() && blank(text.back())) text.remove_suffix(1);
    return std::string(text);
}

std::string ClaimJson(const std::string& ticket, const std::string& receipt) {
    return "{\"ticket\":" + json::Quote(ticket) + ",\"receipt\":" + json::Quote(receipt) + "}";
}

std::string ClaimJson(const TrackedReport& report) { return ClaimJson(report.ticket, report.receipt); }

std::string Base64(const char* data, size_t size) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t at = 0; at < size; at += 3) {
        const size_t left = size - at;
        const uint32_t word = uint32_t(uint8_t(data[at])) << 16 | (left > 1 ? uint32_t(uint8_t(data[at + 1])) << 8 : 0) |
                              (left > 2 ? uint32_t(uint8_t(data[at + 2])) : 0);
        out += kAlphabet[(word >> 18) & 63];
        out += kAlphabet[(word >> 12) & 63];
        out += left > 1 ? kAlphabet[(word >> 6) & 63] : '=';
        out += left > 2 ? kAlphabet[word & 63] : '=';
    }
    return out;
}

struct Answer {
    ReportOutcome failure = ReportOutcome::kSent;
    int status = 0;
    int retry_after_seconds = 0;
    std::optional<json::Value> body;
};

const char* FailureName(HttpFailure failure) {
    switch (failure) {
    case HttpFailure::kNone: return "none";
    case HttpFailure::kTimedOut: return "timed out";
    case HttpFailure::kCertificate: return "certificate";
    case HttpFailure::kInvalidAddress: return "invalid address";
    case HttpFailure::kClientMissing: return "curl missing";
    case HttpFailure::kCancelled: return "cancelled";
    case HttpFailure::kUnreachable: break;
    }
    return "unreachable";
}

// The service's codes and field names are short words; any other text from a body is never copied into the log.
std::string SafeName(std::string_view text, size_t most) {
    if (text.empty()) return "-";
    const auto plain = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == ',' || c == ' '; };
    if (text.size() > most || !std::all_of(text.begin(), text.end(), plain)) return "?";
    std::string out(text);
    std::replace(out.begin(), out.end(), ' ', '_');
    return out;
}

// A reply is trusted only when the transfer finished and the body is JSON.
Answer Ask(const ServiceConfig& config, const char* path, const std::string& body, const char* what) {
    Answer answer;
    if (!config.endpoint.configured()) {
        answer.failure = ReportOutcome::kNotConfigured;
        return answer;
    }
    if (config.cancel && config.cancel->load()) {
        answer.failure = ReportOutcome::kCancelled;
        return answer;
    }
    HttpLimits limits = config.limits;
    limits.cancel = config.cancel.get();
    const HttpReply reply = config.post(config.endpoint.url + path, body, limits);
    if (reply.failure != HttpFailure::kNone) {
        answer.failure = OutcomeOfFailure(reply.failure);
        Say(config, std::string("Bug report: ") + what + " got no answer (" + FailureName(reply.failure) + ")");
        return answer;
    }
    answer.status = reply.status;
    answer.retry_after_seconds = reply.retry_after_seconds;
    answer.body = json::Parse(reply.body);
    if (reply.status < 200 || reply.status >= 300) {
        const bool object = answer.body && answer.body->IsObject();
        answer.failure = OutcomeOfAnswer(reply.status, object ? answer.body->StringOr("error") : "", object ? answer.body->StringOr("detail") : "");
        Say(config, AnswerDiagnostic(what, reply.status, reply.body));
    } else if (!answer.body || !answer.body->IsObject()) {
        answer.failure = ReportOutcome::kServiceError;
        Say(config, std::string("Bug report: ") + what + " failed status=" + std::to_string(reply.status) + " json=no");
    }
    return answer;
}

bool Ok(const Answer& answer) { return answer.failure == ReportOutcome::kSent; }

enum class DumpStep { kDone, kRetry, kStop };

// A connection that failed or a service that faltered may do better on the next try; a refusal or a limit will not.
DumpStep AfterFailure(const Answer& answer) {
    const bool transient = answer.failure == ReportOutcome::kUnreachable || answer.failure == ReportOutcome::kTimedOut ||
                           (answer.failure == ReportOutcome::kServiceError && (answer.status == 0 || answer.status >= 500));
    return transient ? DumpStep::kRetry : DumpStep::kStop;
}

struct DumpProgress {
    bool valid = false;
    uint64_t stored = 0;
    uint64_t bytes = 0;
    bool complete = false;
};

DumpProgress ProgressOf(const Answer& answer, uint64_t expected_bytes) {
    DumpProgress progress;
    if (!answer.body || !answer.body->IsObject()) return progress;
    const json::Value* stored = answer.body->Find("stored");
    const json::Value* bytes = answer.body->Find("bytes");
    if (!stored || !bytes || !stored->IsNumber() || !bytes->IsNumber() || stored->number < 0 || bytes->number < 0) return progress;
    progress.stored = uint64_t(stored->number);
    progress.bytes = uint64_t(bytes->number);
    progress.complete = answer.body->BoolOr("complete");
    progress.valid = progress.bytes == expected_bytes && progress.stored <= expected_bytes;
    return progress;
}

HttpLimits ChunkLimits(HttpLimits limits) {
    limits.send_ms = std::max(limits.send_ms, 60000);
    limits.receive_ms = std::max(limits.receive_ms, 30000);
    limits.total_ms = std::max(limits.total_ms, 90000);
    return limits;
}

// A chunk the service already holds is answered with where its copy ends, and the upload continues from there.
DumpStep SendDumpOnce(const ServiceConfig& config, const std::string& ticket, const std::string& receipt, const CrashDump& dump) {
    std::error_code ec;
    if (fs::file_size(dump.path, ec) != dump.bytes || ec) return DumpStep::kStop;
    const Answer begun = Ask(config, kDumpPath, ClaimJson(ticket, receipt), "crash dump");
    if (!Ok(begun)) return AfterFailure(begun);
    const DumpProgress start = ProgressOf(begun, dump.bytes);
    if (!start.valid) return DumpStep::kStop;
    if (start.complete) return DumpStep::kDone;

    std::ifstream in(dump.path, std::ios::binary);
    if (!in) return DumpStep::kStop;
    ServiceConfig chunks = config;
    chunks.limits = ChunkLimits(config.limits);
    std::vector<char> buffer(kDumpChunkBytes);
    uint64_t offset = start.stored;
    const uint64_t most_requests = dump.bytes / kDumpChunkBytes + 8;
    for (uint64_t request = 0; request < most_requests; ++request) {
        if (offset >= dump.bytes) return DumpStep::kStop;
        const size_t want = size_t(std::min<uint64_t>(kDumpChunkBytes, dump.bytes - offset));
        in.clear();
        in.seekg(std::streamoff(offset));
        in.read(buffer.data(), std::streamsize(want));
        if (size_t(in.gcount()) != want) return DumpStep::kStop;
        const std::string body = "{\"ticket\":" + json::Quote(ticket) + ",\"receipt\":" + json::Quote(receipt) +
                                 ",\"offset\":" + std::to_string(offset) + ",\"data\":\"" + Base64(buffer.data(), want) + "\"}";
        const Answer answer = Ask(chunks, kDumpChunkPath, body, "crash dump chunk");
        const DumpProgress now = ProgressOf(answer, dump.bytes);
        if (answer.status == 409 && now.valid) {
            if (now.complete) return DumpStep::kDone;
            if (now.stored == offset) return DumpStep::kStop;
            offset = now.stored;
            continue;
        }
        if (!Ok(answer)) return AfterFailure(answer);
        if (!now.valid || now.stored <= offset) return DumpStep::kStop;
        if (now.complete) return DumpStep::kDone;
        if (now.stored == dump.bytes) return DumpStep::kStop;
        offset = now.stored;
    }
    return DumpStep::kRetry;
}

} // namespace

ServiceEndpoint ResolveEndpoint(std::string_view built_in, std::string_view override_url) {
    ServiceEndpoint endpoint;
    const std::string requested = TrimmedAscii(override_url);
    const std::string chosen = requested.empty() ? TrimmedAscii(built_in) : requested;
    if (chosen.empty()) {
        endpoint.problem = "No bug report service is set up in this version yet.";
        return endpoint;
    }
    ServiceAddress address;
    if (!ParseServiceAddress(chosen, address) || (address.path != "/" && !address.path.empty())) {
        endpoint.problem = "The bug report service address is not valid.";
        return endpoint;
    }
    endpoint.url = "https://" + address.host + (address.port == 443 ? "" : ":" + std::to_string(address.port));
    return endpoint;
}

ReportOutcome OutcomeOfFailure(HttpFailure failure) {
    switch (failure) {
    case HttpFailure::kNone: return ReportOutcome::kSent;
    case HttpFailure::kTimedOut: return ReportOutcome::kTimedOut;
    case HttpFailure::kCertificate: return ReportOutcome::kCertificate;
    case HttpFailure::kInvalidAddress: return ReportOutcome::kNotConfigured;
    case HttpFailure::kClientMissing: return ReportOutcome::kClientMissing;
    case HttpFailure::kCancelled: return ReportOutcome::kCancelled;
    case HttpFailure::kUnreachable: break;
    }
    return ReportOutcome::kUnreachable;
}

ReportOutcome OutcomeOfAnswer(int status, std::string_view error_code, std::string_view detail) {
    if (status >= 200 && status < 300) return ReportOutcome::kSent;
    if (error_code == "hourly_limit" || error_code == "limit") return ReportOutcome::kRecentLimit;
    if (error_code == "daily_limit") return ReportOutcome::kDailyLimit;
    if (status == 429) return ReportOutcome::kRecentLimit;
    if (status != 400 && status != 403 && status != 413 && status != 415) return ReportOutcome::kServiceError;
    if (error_code == "bad_request" || error_code == "too_large") {
        if (detail == "description" || detail == "description too short") return ReportOutcome::kDescriptionRefused;
        if (detail == "log_text" || detail == "empty log") return ReportOutcome::kLogRefused;
    }
    return ReportOutcome::kRefused;
}

std::string AnswerDiagnostic(std::string_view operation, int status, std::string_view body) {
    const std::optional<json::Value> parsed = json::Parse(body);
    const bool object = parsed && parsed->IsObject();
    const json::Value* protocol = object ? parsed->Find("protocol") : nullptr;
    const bool known_protocol = protocol && protocol->IsNumber() && protocol->number > 0 && protocol->number < 10000;
    return "Bug report: " + std::string(operation) + (status >= 400 && status < 500 ? " rejected" : " failed") +
           " status=" + std::to_string(status) + " code=" + SafeName(object ? parsed->StringOr("error") : "", 32) +
           " detail=" + SafeName(object ? parsed->StringOr("detail") : "", 48) + " json=" + (object ? "yes" : "no") +
           " service_protocol=" + (known_protocol ? std::to_string(protocol->number) : "-");
}

const char* OutcomeTitle(ReportOutcome outcome) {
    switch (outcome) {
    case ReportOutcome::kSent: break;
    case ReportOutcome::kRecentLimit: return "Too many reports were sent recently";
    case ReportOutcome::kDailyLimit: return "Daily report limit reached";
    case ReportOutcome::kUnreachable: return "Cannot reach the report service";
    case ReportOutcome::kTimedOut: return "The report service did not answer in time";
    case ReportOutcome::kCertificate: return "The report service's security certificate was not accepted";
    case ReportOutcome::kRefused: return "The report service did not accept this report";
    case ReportOutcome::kDescriptionRefused: return "The report service did not accept the description";
    case ReportOutcome::kLogRefused: return "The report service did not accept the selected log";
    case ReportOutcome::kServiceError: return "The report service could not take the report";
    case ReportOutcome::kNotConfigured: return "The report service is not available";
    case ReportOutcome::kClientMissing: return "Sending a report needs curl";
    case ReportOutcome::kCancelled: return "The request was cancelled";
    }
    return nullptr;
}

const char* OutcomeAdvice(ReportOutcome outcome) {
    switch (outcome) {
    case ReportOutcome::kSent: break;
    case ReportOutcome::kRecentLimit:
    case ReportOutcome::kDailyLimit: return "Please try again later";
    case ReportOutcome::kUnreachable:
        return "Check your connection and press Send report again; your text is kept. A firewall or security program may be "
               "blocking outgoing HTTPS (port 443).";
    case ReportOutcome::kTimedOut: return "Check your connection and press Send report again; your text is kept.";
    case ReportOutcome::kCertificate:
        return "Nothing was sent. The clock of this PC, a proxy or a security program may be interfering with secure connections.";
    case ReportOutcome::kRefused:
        return "This is not your fault, and changing the text will not help. Your text is kept: save the report on this PC and "
               "attach it to a GitHub issue instead.";
    case ReportOutcome::kDescriptionRefused: return "Change the description, then send it again; your text is kept.";
    case ReportOutcome::kLogRefused: return "Choose another log, then send it again; your text is kept.";
    case ReportOutcome::kServiceError: return "This is not your fault. Try again in a few minutes; your text is kept.";
    case ReportOutcome::kNotConfigured:
        return "Nothing was sent. This version has no valid report service address (SOTB_BUG_REPORT_URL overrides it). Save the report on this PC and attach it to a GitHub issue instead.";
    case ReportOutcome::kClientMissing:
        return "Nothing was sent. Install curl from your distribution's packages and press Send report again, or save the "
               "report on this PC; your text is kept.";
    case ReportOutcome::kCancelled: return "The launcher was closed before the service answered.";
    }
    return nullptr;
}

std::string RetryAfterText(int seconds) {
    if (seconds <= 0) return {};
    if (seconds < 90) return "Try again in about " + std::to_string(seconds) + " seconds.";
    const int minutes = (seconds + 30) / 60;
    if (minutes < 90) return "Try again in about " + std::to_string(minutes) + " minutes.";
    return "Try again in about " + std::to_string((minutes + 30) / 60) + " hours.";
}

std::string NewSubmissionKey() {
    uint8_t bytes[32];
    if (!crypto::RandomBytes(bytes, sizeof(bytes))) return {};
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string key;
    for (const uint8_t byte : bytes) {
        key += kDigits[byte >> 4];
        key += kDigits[byte & 15];
    }
    return key;
}

DumpUpload UploadCrashDump(const ServiceConfig& config, const std::string& ticket, const std::string& receipt, const CrashDump& dump) {
    if (!IsTicket(ticket) || !IsReceipt(receipt) || dump.sha256.empty()) return DumpUpload::kFailed;
    for (int attempt = 0; attempt < kDumpUploadAttempts; ++attempt) {
        const DumpStep step = SendDumpOnce(config, ticket, receipt, dump);
        if (step == DumpStep::kDone) {
            Say(config, "Bug report: crash dump " + dump.name + " uploaded for " + ticket);
            return DumpUpload::kUploaded;
        }
        if (step == DumpStep::kStop) break;
    }
    Say(config, "Bug report: crash dump " + dump.name + " was not uploaded for " + ticket);
    return DumpUpload::kFailed;
}

ReportReceipt FileReport(const ServiceConfig& config, const IncidentPackage& package, std::string_view submission) {
    ReportReceipt receipt;
    const Answer answer = Ask(config, kReportsPath, IncidentRequestJson(package, submission), "send");
    if (!Ok(answer)) {
        receipt.outcome = answer.failure;
        receipt.retry_after_seconds = answer.retry_after_seconds;
        return receipt;
    }
    const std::string ticket = answer.body->StringOr("ticket");
    const std::string credential = answer.body->StringOr("receipt");
    if (!IsTicket(ticket) || !IsReceipt(credential)) {
        Say(config, "Bug report: send failed status=" + std::to_string(answer.status) + " json=yes reason=no valid Report ID");
        receipt.outcome = ReportOutcome::kServiceError;
        return receipt;
    }
    receipt.outcome = ReportOutcome::kSent;
    receipt.ticket = ticket;
    receipt.credential = credential;
    receipt.submitted_at = answer.body->NumberOr("submitted_at");
    receipt.dump_expected = package.crash_dump.has_value() && answer.body->StringOr("dump") == "expected";
    if (package.crash_dump_requested && !package.crash_dump) receipt.dump = DumpUpload::kNotAttached;
    return receipt;
}

StatusCheck FetchReportStatuses(const ServiceConfig& config, const std::vector<TrackedReport>& reports) {
    StatusCheck check;
    std::string claims;
    for (size_t i = 0; i < std::min(reports.size(), kMostTrackedReports); ++i) claims += (claims.empty() ? "" : ",") + ClaimJson(reports[i]);
    if (claims.empty()) return check;
    const Answer answer = Ask(config, kStatusPath, "{\"reports\":[" + claims + "]}", "status");
    if (!Ok(answer)) {
        check.failure = answer.failure;
        return check;
    }
    const json::Value* list = answer.body->Find("reports");
    if (!list || !list->IsArray()) {
        check.failure = ReportOutcome::kServiceError;
        return check;
    }
    check.reached = true;
    for (const json::Value& entry : list->array) {
        if (!entry.IsObject() || !IsTicket(entry.StringOr("ticket"))) continue;
        check.statuses.push_back({entry.StringOr("ticket"), entry.StringOr("status"), entry.NumberOr("status_updated_at"),
                                  entry.StringOr("fixed_version"), entry.StringOr("player_message"), entry.NumberOr("message_updated_at"),
                                  entry.StringOr("test_feedback")});
    }
    return check;
}

FeedbackAnswer SendTestFeedback(const ServiceConfig& config, const TrackedReport& report, std::string_view result, const ReportFacts& facts) {
    if (result != kTestFixed && result != kTestNotFixed) return {FeedbackOutcome::kFailed, "result"};
    const std::string body = "{\"ticket\":" + json::Quote(report.ticket) + ",\"receipt\":" + json::Quote(report.receipt) +
                             ",\"revision\":" + std::to_string(report.status_updated_at) + ",\"result\":" + json::Quote(std::string(result)) +
                             ",\"launcher_version\":" + json::Quote(facts.launcher_version) + ",\"game_build\":" + json::Quote(facts.game_build) + "}";
    const Answer answer = Ask(config, kFeedbackPath, body, "player test answer");
    if (Ok(answer)) {
        const bool confirmed = answer.body->StringOr("ticket") == report.ticket && answer.body->NumberOr("revision") == report.status_updated_at &&
                               answer.body->StringOr("test_feedback") == result;
        return confirmed ? FeedbackAnswer{FeedbackOutcome::kSaved, "ok"} : FeedbackAnswer{FeedbackOutcome::kFailed, "malformed_reply"};
    }
    const bool object = answer.body && answer.body->IsObject();
    const std::string error = object ? answer.body->StringOr("error") : std::string();
    const std::string code = error.empty() ? std::string() : SafeName(error, 32);
    if (answer.status == 404 || code == "stale_request" || code == "already_answered") {
        return {FeedbackOutcome::kStale, code.empty() ? "not_found" : code};
    }
    const bool unreachable = answer.failure == ReportOutcome::kUnreachable || answer.failure == ReportOutcome::kTimedOut ||
                             answer.failure == ReportOutcome::kCertificate || answer.failure == ReportOutcome::kClientMissing;
    return {unreachable ? FeedbackOutcome::kUnreachable : FeedbackOutcome::kFailed, code.empty() ? "no_answer" : code};
}

SentReportRead FetchSentReport(const ServiceConfig& config, const TrackedReport& report) {
    SentReportRead read;
    const Answer answer = Ask(config, kReadPath, ClaimJson(report), "read");
    if (answer.status == 404) {
        read.reached = true;
        return read;
    }
    if (!Ok(answer)) {
        read.failure = answer.failure;
        return read;
    }
    const json::Value* stored = answer.body->Find("report");
    if (!stored || !stored->IsObject()) {
        read.failure = ReportOutcome::kServiceError;
        return read;
    }
    read.reached = true;
    read.package = SentReportOf(*stored);
    return read;
}

TrackedReport TrackedReportOf(const IncidentPackage& package, const ReportReceipt& receipt) {
    TrackedReport report;
    report.ticket = receipt.ticket;
    report.receipt = receipt.credential;
    report.submitted_at = receipt.submitted_at;
    report.status_updated_at = receipt.submitted_at;
    report.description = package.Value("description");
    report.log_file = package.Value("log_file");
    report.launcher_version = package.Value("launcher_version");
    report.game_build = package.Value("game_build");
    report.game_version = package.Value("game_version");
    report.region = package.Value("region");
    report.operating_system = package.Value("operating_system");
    report.renderer = package.Value("renderer");
    report.gpu = package.Value("gpu");
    if (receipt.dump == DumpUpload::kUploaded) report.crash_dump = kDumpUploaded;
    if (receipt.dump == DumpUpload::kFailed) report.crash_dump = kDumpNotUploaded;
    return report;
}

} // namespace Sotb
