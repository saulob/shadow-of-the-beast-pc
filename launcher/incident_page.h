#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "incident_client.h"
#include "incident_report.h"
#include "my_reports_page.h"
#include "report_send_state.h"
#include "trg/launcher.h"

namespace Sotb {

// The launcher's Bug Report page: report the problem, choose one game log, preview what will be sent, send it. Nothing is
// sent until the player presses Send report. A connection is opened to ask about reports already sent, and only when there
// are some, and to send an answer to a player test, only from its buttons.
class IncidentPage {
public:
    struct Places {
        std::filesystem::path logs_dir;
        std::filesystem::path running_log;
        std::filesystem::path user_dir;
        std::vector<std::filesystem::path> install_folders;
        std::function<void(const std::string&)> log; // called on the UI thread only
    };

    explicit IncidentPage(Places places);
    // Cancels the requests still running; their threads end on their own and drop what they find.
    ~IncidentPage();
    IncidentPage(const IncidentPage&) = delete;
    IncidentPage& operator=(const IncidentPage&) = delete;

    void Draw(trg::Ui& ui, const ReportFacts& facts);
    // Asks for the statuses of the reports in My Reports; nothing is asked when there are none.
    void CheckReports() { my_reports_.Refresh(service_); }
    bool HasReportUpdates() { return my_reports_.HasUpdates(); }

private:
    // Shared with the sending thread, which may outlive the launcher window.
    struct Delivery {
        std::atomic<bool> done{false};
        std::atomic<bool> uploading_dump{false};
        ReportReceipt receipt;
        std::shared_ptr<const IncidentPackage> package;
    };
    struct DumpRetry {
        std::atomic<bool> done{false};
        DumpUpload outcome = DumpUpload::kFailed;
    };
    struct LogScan {
        std::atomic<bool> done{false};
        std::vector<GameLog> logs;
    };

    SendState State() const;
    void StartLogScan();
    void CollectLogScan();
    void SelectLog(size_t index);
    void DraftChanged();
    void ForgetPreview();
    bool AcceptDraft();
    void CollectDelivery();
    void CollectDumpRetry();
    void RetryDump();
    std::shared_ptr<const IncidentPackage> BuildPackage(const ReportFacts& facts);
    void TogglePreview(const ReportFacts& facts);
    void Send(const ReportFacts& facts);
    void Save(const ReportFacts& facts);
    void DrawDescription(trg::Ui& ui, bool sending);
    void DrawLogs(trg::Ui& ui, bool sending);
    void DrawCrashDump(trg::Ui& ui, bool sending);
    void DrawDumpResult(trg::Ui& ui);
    void DrawPreview(trg::Ui& ui, const ReportFacts& facts, bool sending);
    void DrawSend(trg::Ui& ui, const ReportFacts& facts, bool sending);
    void DrawOtherOptions(trg::Ui& ui, const ReportFacts& facts, bool sending);
    void DrawResult(trg::Ui& ui);
    void DrawSaved(trg::Ui& ui);

    void Log(const std::string& line) const {
        if (places_.log) places_.log(line);
    }

    Places places_;
    ServiceConfig service_;
    char description_[kMostDescriptionCharacters * 4 + 1] = {};
    std::vector<GameLog> logs_;
    bool logs_found_ = false;
    std::shared_ptr<LogScan> scan_;
    int last_drawn_frame_ = -2;
    float form_top_ = 0.0f;
    bool new_report_requested_ = false;
    bool focus_description_ = false;
    size_t selected_ = 0;
    DraftProblem problem_ = DraftProblem::kNone;
    double problem_time_ = 0.0;
    bool bring_problem_into_view_ = false;
    std::shared_ptr<const IncidentPackage> previewed_;
    std::string preview_text_;
    bool log_failed_ = false;
    std::shared_ptr<Delivery> delivery_;
    std::shared_ptr<DumpRetry> dump_retry_;
    bool attach_crash_dump_ = false;
    std::string submission_; // one per draft, so a repeated send never files a second report
    std::optional<CrashDump> sent_dump_;
    std::optional<ReportReceipt> result_;
    bool sent_report_remembered_ = true;
    std::filesystem::path saved_report_;
    bool save_failed_ = false;
    bool bring_result_into_view_ = false;
    bool bring_saved_into_view_ = false;
    MyReports my_reports_;
};

} // namespace Sotb
