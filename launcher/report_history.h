// My Reports: the reports this PC sent, kept in the user folder with the receipt that proves ownership of each.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Sotb {

// Matches REPORT.statusBatch in server/shadow-bug-reports/src/reports.js.
inline constexpr size_t kMostTrackedReports = 30;
inline constexpr char kReportHistoryFile[] = "bug_reports.json";

inline constexpr char kAwaitingPlayerTest[] = "awaiting_player_test";
inline constexpr char kTestFixed[] = "fixed";
inline constexpr char kTestNotFixed[] = "not_fixed";
inline constexpr char kDumpUploaded[] = "uploaded";
inline constexpr char kDumpNotUploaded[] = "not_uploaded";

struct TrackedReport {
    std::string ticket;
    std::string receipt;
    int64_t submitted_at = 0;
    std::string description;
    std::string log_file;
    std::string launcher_version;
    std::string game_build;
    std::string game_version;
    std::string region;
    std::string operating_system;
    std::string renderer;
    std::string gpu;
    std::string status = "new";
    int64_t status_updated_at = 0;
    std::string fixed_version;
    std::string player_message;
    int64_t message_updated_at = 0; // 0 when the service did not say
    // The player's answer to the test request whose status time is test_feedback_revision.
    std::string test_feedback;
    int64_t test_feedback_revision = 0;
    std::string crash_dump; // kDumpUploaded or kDumpNotUploaded; empty when the report announced none
    bool unseen = false;
    // The developer message appeared or changed and the report has not been opened since.
    bool message_unseen = false;
    // The service answered a check without this report, so it was deleted there. Its last known state stays.
    bool unavailable = false;
};

// What the service answers for one report.
struct ReportStatusUpdate {
    std::string ticket;
    std::string status;
    int64_t updated_at = 0;
    std::string fixed_version;
    std::string player_message;
    int64_t message_updated_at = 0;
    std::string test_feedback; // fixed or not_fixed; empty while unanswered or from a service without player tests
};

enum class PlayerTest { kNone, kAwaitingAnswer, kConfirmedFixed, kStillHappening };
// An answer counts only for the request it was given to: a newer request (a later status time) is open again.
PlayerTest PlayerTestOf(const TrackedReport& report);

// Indexes into `reports`: unread replies first, then the rest, each newest first. `open_reply` keeps its place while it is
// open, although opening it marked the reply read.
std::vector<size_t> ReportDisplayOrder(const std::vector<TrackedReport>& reports, std::string_view open_reply = {});

// English, for Tr; nullptr for a status this launcher does not know.
const char* ReportStatusText(std::string_view status);
// "Investigating", or "Fixed in version 0.1.1"; translated.
std::string ReportStatusLine(const TrackedReport& report);
// The start of the description on one line.
std::string ReportPreview(std::string_view description);

class ReportHistory {
public:
    explicit ReportHistory(std::filesystem::path file) : file_(std::move(file)) {}

    // A damaged file is set aside, not overwritten, and the history starts empty.
    void Load();
    // Written to a new file and moved into place, so an interrupted save leaves the previous history.
    bool Save() const;

    const std::vector<TrackedReport>& reports() const { return reports_; }
    bool HasUnseen() const;
    bool HasUnseenMessage() const;
    // The report to show first: one with an unseen developer message before one with only a status change, the most recently
    // updated first. Empty when nothing is unseen.
    std::string NextUnseen() const;

    // The history changes in memory even when saving fails; false says it was not saved.
    bool Add(TrackedReport report);
    // Newer facts for the reports held here; a difference marks the report unseen, and a new developer message (other text,
    // or another revision of it) also marks its message unseen. A report in `asked` that the answer leaves out is marked
    // unavailable, once, and stops counting as an update. False when nothing changed.
    bool Apply(const std::vector<ReportStatusUpdate>& statuses, const std::vector<std::string>& asked = {});
    bool MarkSeen(std::string_view ticket);
    // False when the report is no longer at that open request.
    bool RecordTestFeedback(std::string_view ticket, int64_t revision, std::string_view result);
    // False for an unknown report or state, or when nothing changed.
    bool SetCrashDump(std::string_view ticket, std::string_view state);
    bool Remove(std::string_view ticket);

    // The saved text of the history; the receipts are in it.
    std::string Dump() const;

private:
    std::filesystem::path file_;
    std::vector<TrackedReport> reports_;
};

} // namespace Sotb
