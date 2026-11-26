// The My Reports section of the Bug Report page. Checks run off the UI thread and ask only about the reports listed.
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "incident_client.h"
#include "report_history.h"
#include "trg/launcher.h"

namespace Sotb {

class MyReports {
public:
    explicit MyReports(std::filesystem::path history_file);

    // `facts` are the launcher version and game build sent with an answer to a player test.
    void Draw(trg::Ui& ui, const ServiceConfig& service, const ReportFacts& facts);
    // Asks for the newest statuses unless a check is running or there is nothing to ask about. A failed check keeps the last
    // known statuses.
    void Refresh(const ServiceConfig& service, bool asked_by_player = false);
    // The same, unless a check started less than `interval` ago.
    void RefreshIfStale(const ServiceConfig& service, std::chrono::steady_clock::duration interval);
    bool Checking() const { return check_ != nullptr; }
    // True once after the player pressed Send a new report: the page then shows its form, which sends nothing by itself.
    bool TakeNewReportRequest() { return std::exchange(new_report_requested_, false); }
    // False when the report could not be added to the saved history; the report itself was sent.
    bool Remember(const IncidentPackage& package, const ReportReceipt& receipt);
    void NoteCrashDump(const std::string& ticket, DumpUpload outcome);
    bool HasUpdates();
    // An unseen developer message, which the page shows before a plain status change.
    bool HasNewReply();
    // Opens the report the page notice is about: a reply first, then the most recently updated.
    void ShowUpdate();
    // Applies what finished checks and reads found; called each frame by Draw.
    void Collect();

private:
    // Shared with the thread that asks, which may outlive the launcher window.
    struct Check {
        std::atomic<bool> done{false};
        StatusCheck result;
        std::vector<std::string> asked;
    };
    struct Read {
        std::atomic<bool> done{false};
        std::string ticket;
        SentReportRead result;
    };
    struct FeedbackSend {
        std::atomic<bool> done{false};
        std::string ticket;
        int64_t revision = 0;
        std::string result;
        FeedbackAnswer answer;
    };

    void SaveHistory();
    void CollectFeedback();
    void SendFeedback(const TrackedReport& report, std::string_view result, const ServiceConfig& service);
    void DrawPlayerTest(trg::Ui& ui, const TrackedReport& report, const ServiceConfig& service);
    void Open(const std::string& ticket);
    void Collapse();
    void ForgetSentReport();
    void ReadSentReport(const TrackedReport& report, const ServiceConfig& service);
    void DrawHeader(trg::Ui& ui, const ServiceConfig& service);
    void DrawReports(trg::Ui& ui, const ServiceConfig& service);
    void DrawRow(trg::Ui& ui, const TrackedReport& report, size_t index, std::string& toggled);
    void DrawDetails(trg::Ui& ui, const TrackedReport& report, size_t index, const ServiceConfig& service, std::string& toggled,
                     std::string& removed);
    void DrawActions(trg::Ui& ui, const TrackedReport& report, const ServiceConfig& service, std::string& removed);
    void DrawSentReport(trg::Ui& ui);
    bool ConfirmRemoval(trg::Ui& ui);

    ReportHistory history_;
    std::shared_ptr<Check> check_;
    ReportOutcome check_failure_ = ReportOutcome::kSent;
    bool check_failed_ = false;
    bool asked_by_player_ = false;
    std::optional<std::chrono::steady_clock::time_point> last_check_;
    bool bring_detail_into_view_ = false;
    std::string open_ticket_;
    // The open report whose developer message was unread when it was opened; keeps its "New reply" cue until it closes.
    std::string fresh_reply_;
    ReportFacts facts_;
    std::shared_ptr<FeedbackSend> feedback_;
    // The report whose last answer could not be sent, and why; cleared by the next try.
    std::string feedback_error_ticket_;
    FeedbackOutcome feedback_error_ = FeedbackOutcome::kFailed;
    bool refresh_wanted_ = false;
    bool new_report_requested_ = false;
    std::shared_ptr<Read> reading_;
    std::string sent_ticket_;
    std::string sent_text_;
    bool sent_gone_ = false;
    bool sent_failed_ = false;
};

} // namespace Sotb
