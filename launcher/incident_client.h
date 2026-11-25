// The Bug Report page's calls to the report service (server/shadow-bug-reports). Each opens one short HTTPS request and
// blocks: run them off the UI thread.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "http_post.h"
#include "incident_report.h"
#include "report_history.h"

namespace Sotb {

// Overrides kBugReportServiceUrl (game_profile.h) for one run.
inline constexpr char kBugReportUrlVariable[] = "SOTB_BUG_REPORT_URL";

struct ServiceEndpoint {
    std::string url;     // scheme://host[:port], no trailing slash; empty when there is no usable service
    std::string problem; // English, for Tr; why `url` is empty
    bool configured() const { return !url.empty(); }
};

// `override_url` (the environment) wins over `built_in` when it is not empty.
ServiceEndpoint ResolveEndpoint(std::string_view built_in, std::string_view override_url);

struct ServiceConfig {
    ServiceEndpoint endpoint;
    PostJson post = PostJsonHttps;
    HttpLimits limits;
    std::shared_ptr<std::atomic<bool>> cancel; // set when the launcher closes
    std::function<void(const std::string&)> log; // never receives a receipt or any report text; called from worker threads
};

// kRefused: the service refused something the player cannot change (a field the launcher fills, an unknown field, the
// request's form). kDescriptionRefused and kLogRefused name what the player can change.
enum class ReportOutcome {
    kSent,
    kRecentLimit,
    kDailyLimit,
    kUnreachable,
    kTimedOut,
    kCertificate,
    kRefused,
    kDescriptionRefused,
    kLogRefused,
    kServiceError,
    kNotConfigured,
    kClientMissing,
    kCancelled
};

// What a failed transfer or an answer means for the page. `detail` is the service's own name for what it refused.
ReportOutcome OutcomeOfFailure(HttpFailure failure);
ReportOutcome OutcomeOfAnswer(int status, std::string_view error_code, std::string_view detail = {});
// One log line about an answer that was not a success: status, the service's error code and detail, whether the body was
// JSON, and the service's protocol. Only short ASCII names from the body are kept, never report text.
std::string AnswerDiagnostic(std::string_view operation, int status, std::string_view body);
// What the page tells the player about a request that did not go through. English, for Tr; nullptr for kSent.
const char* OutcomeTitle(ReportOutcome outcome);
const char* OutcomeAdvice(ReportOutcome outcome);

// kNotAttached: the player asked for the dump but it failed the local check, so only the report went. kFailed: the report
// was filed and its dump did not arrive; another try resumes under the same Report ID.
enum class DumpUpload { kNotRequested, kNotAttached, kUploaded, kFailed };

struct ReportReceipt {
    ReportOutcome outcome = ReportOutcome::kServiceError;
    std::string ticket;
    // Proves ownership of this one report. Never logged.
    std::string credential;
    int64_t submitted_at = 0;
    int retry_after_seconds = 0; // when the service refused with a limit and said when to try again
    bool dump_expected = false;  // the service took the announced dump and waits for its bytes
    DumpUpload dump = DumpUpload::kNotRequested;
};

// "Try again in about 5 minutes." for what the service asked; empty for 0.
std::string RetryAfterText(int seconds);

// 64 random lowercase hex digits; empty when the system has no random source. One per draft: a request repeated with it,
// after an answer that never arrived, returns the report already filed instead of a second one.
std::string NewSubmissionKey();

// Sends the report and closes. Nothing is sent, and no connection opened, without a configured service.
ReportReceipt FileReport(const ServiceConfig& config, const IncidentPackage& package, std::string_view submission = {});

inline constexpr size_t kDumpChunkBytes = 1024 * 1024;
inline constexpr int kDumpUploadAttempts = 3;

// Sends the dump of a filed report in chunks read from the file, from the first byte the service does not hold yet. The
// receipt is the only proof sent, so it never files a second report. Blocks.
DumpUpload UploadCrashDump(const ServiceConfig& config, const std::string& ticket, const std::string& receipt, const CrashDump& dump);

struct StatusCheck {
    bool reached = false;
    ReportOutcome failure = ReportOutcome::kSent; // what stopped it, when `reached` is false
    std::vector<ReportStatusUpdate> statuses;
};
// The receipts are the only proof sent; at most kMostTrackedReports are asked about.
StatusCheck FetchReportStatuses(const ServiceConfig& config, const std::vector<TrackedReport>& reports);

struct SentReportRead {
    bool reached = false;
    ReportOutcome failure = ReportOutcome::kSent;
    std::optional<IncidentPackage> package; // empty when the service no longer has the report
};
SentReportRead FetchSentReport(const ServiceConfig& config, const TrackedReport& report);

// What My Reports keeps of a sent report.
TrackedReport TrackedReportOf(const IncidentPackage& package, const ReportReceipt& receipt);

} // namespace Sotb
