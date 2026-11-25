// What the Bug Report page shows while and after a report is sent: one state at a time from what is in flight and the
// last receipt, Send refused while busy, the crash dump's own outcome and retry, and failure texts free of private
// details. Pure state plus source wiring; no window, no network.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>

#include "report_send_state.h"
#include "test_progress.h"

namespace fs = std::filesystem;
using namespace Sotb;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                          \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(condition)) {                                                       \
            ++g_failures;                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
        }                                                                         \
        sotb_test_progress(g_checks);                                             \
    } while (0)

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}

bool Contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

std::string Between(const std::string& text, const char* from, const char* to) {
    const size_t begin = text.find(from);
    if (begin == std::string::npos) return {};
    const size_t end = text.find(to, begin + 1);
    return text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

ReportReceipt Receipt(ReportOutcome outcome, DumpUpload dump = DumpUpload::kNotRequested) {
    ReportReceipt receipt;
    receipt.outcome = outcome;
    receipt.dump = dump;
    if (outcome == ReportOutcome::kSent) {
        receipt.ticket = "SOTB-7K4M9";
        receipt.credential = "secret-receipt";
    }
    return receipt;
}

SendState With(const ReportReceipt& receipt) {
    SendFacts facts;
    facts.result = &receipt;
    return ClassifySend(facts);
}

constexpr ReportOutcome kFailures[] = {ReportOutcome::kRecentLimit, ReportOutcome::kDailyLimit,  ReportOutcome::kUnreachable,
                                       ReportOutcome::kTimedOut,    ReportOutcome::kCertificate, ReportOutcome::kRefused,
                                       ReportOutcome::kDescriptionRefused, ReportOutcome::kLogRefused, ReportOutcome::kServiceError,
                                       ReportOutcome::kNotConfigured, ReportOutcome::kClientMissing, ReportOutcome::kCancelled};

void TestIdle() {
    CHECK(ClassifySend({}) == SendState::kIdle);
    CHECK(CanSend(SendState::kIdle, true) && !CanSend(SendState::kIdle, false));
    CHECK(!SendBusy(SendState::kIdle));
}

void TestSending() {
    SendFacts facts;
    facts.delivering = true;
    CHECK(ClassifySend(facts) == SendState::kSending);
    CHECK(SendBusy(SendState::kSending) && !CanSend(SendState::kSending, true));
    // A previous result does not hide that a new report is on its way.
    const ReportReceipt old = Receipt(ReportOutcome::kUnreachable);
    facts.result = &old;
    CHECK(ClassifySend(facts) == SendState::kSending);
}

void TestSuccess() {
    const ReportReceipt sent = Receipt(ReportOutcome::kSent);
    CHECK(With(sent) == SendState::kSent && !SendBusy(SendState::kSent));
    CHECK(!sent.ticket.empty());
    const ReportReceipt uploaded = Receipt(ReportOutcome::kSent, DumpUpload::kUploaded);
    CHECK(With(uploaded) == SendState::kSent);
    CHECK(!CanRetryDump(With(uploaded), true, true));
}

void TestDumpUploadState() {
    SendFacts facts;
    facts.delivering = true;
    facts.uploading_dump = true;
    CHECK(ClassifySend(facts) == SendState::kUploadingDump);
    SendFacts retry;
    const ReportReceipt failed = Receipt(ReportOutcome::kSent, DumpUpload::kFailed);
    retry.retrying_dump = true;
    retry.result = &failed;
    CHECK(ClassifySend(retry) == SendState::kUploadingDump);
    CHECK(SendBusy(SendState::kUploadingDump) && !CanSend(SendState::kUploadingDump, true));
    CHECK(!CanRetryDump(SendState::kUploadingDump, true, true));
}

void TestDumpFailureKeepsReport() {
    const ReportReceipt receipt = Receipt(ReportOutcome::kSent, DumpUpload::kFailed);
    CHECK(With(receipt) == SendState::kSentDumpFailed);
    CHECK(With(receipt) != SendState::kFailed);
    CHECK(!SendBusy(SendState::kSentDumpFailed));
    // An invalid dump that was never attached leaves a plain success.
    CHECK(With(Receipt(ReportOutcome::kSent, DumpUpload::kNotAttached)) == SendState::kSent);
}

void TestDumpRetryOnlyWhenMeaningful() {
    CHECK(CanRetryDump(SendState::kSentDumpFailed, true, true));
    CHECK(!CanRetryDump(SendState::kSentDumpFailed, false, true));   // no dump kept to send again
    CHECK(!CanRetryDump(SendState::kSentDumpFailed, true, false));   // no receipt to prove the report
    for (const SendState state : {SendState::kIdle, SendState::kSending, SendState::kUploadingDump, SendState::kSent, SendState::kFailed}) {
        CHECK(!CanRetryDump(state, true, true));
    }
}

void TestReportFailure() {
    for (const ReportOutcome outcome : kFailures) {
        const ReportReceipt receipt = Receipt(outcome);
        CHECK(With(receipt) == SendState::kFailed);
        CHECK(CanSend(SendState::kFailed, true));
        CHECK(!CanRetryDump(SendState::kFailed, true, true));
    }
}

void TestFailureTexts() {
    CHECK(Contains(OutcomeTitle(ReportOutcome::kRecentLimit), "Too many reports"));
    CHECK(Contains(OutcomeTitle(ReportOutcome::kDailyLimit), "limit"));
    CHECK(Contains(OutcomeAdvice(ReportOutcome::kRecentLimit), "try again later"));
    CHECK(RetryAfterText(300) == "Try again in about 5 minutes.");
    CHECK(RetryAfterText(0).empty());
    CHECK(Contains(OutcomeTitle(ReportOutcome::kServiceError), "could not take the report"));
    CHECK(Contains(OutcomeAdvice(ReportOutcome::kServiceError), "not your fault"));
    CHECK(Contains(OutcomeTitle(ReportOutcome::kNotConfigured), "not available"));
    CHECK(Contains(OutcomeTitle(ReportOutcome::kUnreachable), "Cannot reach"));
    CHECK(Contains(OutcomeTitle(ReportOutcome::kRefused), "did not accept this report"));
    CHECK(Contains(OutcomeTitle(ReportOutcome::kDescriptionRefused), "description"));
    CHECK(Contains(OutcomeTitle(ReportOutcome::kLogRefused), "selected log"));
    // A refusal the player cannot fix never asks for a new description or log.
    CHECK(!Contains(OutcomeAdvice(ReportOutcome::kRefused), "Change the description") && !Contains(OutcomeAdvice(ReportOutcome::kRefused), "another log"));
    CHECK(Contains(OutcomeAdvice(ReportOutcome::kRefused), "text is kept"));
    CHECK(OutcomeTitle(ReportOutcome::kSent) == nullptr);

    // Every failure has a short title and advice, with no path, credential, HTTP detail or stack in them.
    for (const ReportOutcome outcome : kFailures) {
        const std::string title = OutcomeTitle(outcome) ? OutcomeTitle(outcome) : "";
        const std::string advice = OutcomeAdvice(outcome) ? OutcomeAdvice(outcome) : "";
        CHECK(!title.empty() && !advice.empty());
        CHECK(title.size() < 100 && advice.size() < 300);
        for (const std::string* text : {&title, &advice}) {
            CHECK(!Contains(*text, ":\\") && !Contains(*text, "\\") && !Contains(*text, "/Users") && !Contains(*text, "/home"));
            CHECK(!Contains(*text, "token") && !Contains(*text, "Token") && !Contains(*text, "credential") &&
                  !Contains(*text, "receipt") && !Contains(*text, "Bearer") && !Contains(*text, "Authorization"));
            CHECK(!Contains(*text, "HTTP/") && !Contains(*text, "{\"") && !Contains(*text, "stack") && !Contains(*text, "worker"));
        }
    }
}

void TestPageWiring() {
    const fs::path root = SOTB_SOURCE_DIR;
    const std::string page = ReadAll(root / "launcher/incident_page.cpp");
    CHECK(!page.empty());
    const std::string send = Between(page, "void IncidentPage::Send(", "void IncidentPage::Save(");
    const std::string collect = Between(page, "void IncidentPage::CollectDelivery()", "void IncidentPage::CollectDumpRetry()");
    const std::string retry = Between(page, "void IncidentPage::RetryDump()", "std::shared_ptr<const IncidentPackage> IncidentPage::BuildPackage(");
    const std::string draw_send = Between(page, "void IncidentPage::DrawSend(", "void IncidentPage::DrawOtherOptions(");
    const std::string result = Between(page, "void IncidentPage::DrawResult(", "void IncidentPage::DrawDumpResult(");
    const std::string dump = Between(page, "void IncidentPage::DrawDumpResult(", "void IncidentPage::DrawSaved(");
    CHECK(!send.empty() && !collect.empty() && !retry.empty() && !draw_send.empty() && !result.empty() && !dump.empty());

    // The page and Send read one state; a repeated Send while busy files nothing.
    CHECK(Contains(page, "const bool sending = SendBusy(State());"));
    CHECK(send.find("if (!CanSend(State(), service_.endpoint.configured()) || !AcceptDraft()) return;") <
          send.find("FileReport(service, *delivery->package, submission)"));
    CHECK(Contains(draw_send, "ImGui::BeginDisabled(sending || !service_.endpoint.configured());"));
    CHECK(Contains(draw_send, "sending ? \"Sending...\" : \"Send report\""));

    // One submission key per draft: a retry after a failure sends the same key, so the service files it once.
    CHECK(Contains(send, "if (submission_.empty()) submission_ = NewSubmissionKey();"));
    CHECK(Contains(Between(page, "void IncidentPage::DraftChanged()", "void IncidentPage::ForgetPreview()"), "submission_.clear();"));

    // Success clears the draft (so it cannot go twice) but keeps the result and its Report ID on screen; a failure
    // keeps the text for another try.
    const size_t sent_branch = collect.find("if (result_->outcome == ReportOutcome::kSent) {");
    const size_t else_branch = collect.find("} else {", sent_branch);
    const size_t cleared = collect.find("description_[0] = '\\0';", sent_branch);
    CHECK(sent_branch != std::string::npos && cleared < else_branch);
    // Exactly one success line in the launcher: the page's, written once per collected delivery.
    const std::string client = ReadAll(root / "launcher/incident_client.cpp");
    const auto count = [](const std::string& text, const char* needle) {
        size_t n = 0;
        for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
        return n;
    };
    CHECK(count(page, "\"Bug report: sent as ") == 1 && count(client, "\"Bug report: sent as ") == 0);
    CHECK(collect.find("\"Bug report: sent as ", sent_branch) < else_branch);
    CHECK(collect.find("description_", else_branch) == std::string::npos);
    CHECK(!Contains(collect, "result_.reset()"));
    CHECK(Contains(result, "\"Report sent successfully\"") && Contains(result, "result_->ticket"));
    CHECK(Contains(result, "case SendState::kSentDumpFailed: break;"));

    // Busy states first, then the failure card with mapped title and advice; nothing private is printed.
    CHECK(Contains(result, "case SendState::kUploadingDump:") && Contains(result, "case SendState::kSending:"));
    CHECK(Contains(result, "OutcomeTitle(result_->outcome)") && Contains(result, "OutcomeAdvice(result_->outcome)"));
    CHECK(!Contains(result, "credential") && !Contains(result, "path") && !Contains(dump, "credential.c_str()"));
    CHECK(!Contains(page, "credential +") && !Contains(page, "+ result_->credential"));

    // The dump is retried alone under the existing Report ID, only when that is meaningful.
    CHECK(Contains(retry, "UploadCrashDump(service, ticket, receipt, dump)") && !Contains(retry, "FileReport("));
    CHECK(Contains(retry, "CanRetryDump(State(), sent_dump_.has_value(), !result_->credential.empty())"));
    CHECK(Contains(dump, "CanRetryDump(State(), sent_dump_.has_value(), !result_->credential.empty())"));
    CHECK(Contains(dump, "\"The report was sent, but its crash dump could not be uploaded.\""));
}

} // namespace

int main() {
    TestIdle();
    TestSending();
    TestSuccess();
    TestDumpUploadState();
    TestDumpFailureKeepsReport();
    TestDumpRetryOnlyWhenMeaningful();
    TestReportFailure();
    TestFailureTexts();
    TestPageWiring();
    std::printf("bug report feedback tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
