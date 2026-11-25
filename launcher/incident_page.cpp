#include "incident_page.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>

#include "game_profile.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "localization/ui_text.h"
#include "page_margin.h"
#include "report_send_state.h"
#include "report_ui.h"
#include "trg/platform.h"
#include "utf8.h"

namespace Sotb {
namespace {

constexpr float kDescriptionHeight = 112.0f;
constexpr float kPreviewHeight = 300.0f;
constexpr float kAlertTint = 0.16f, kAlertFlashTint = 0.5f;
constexpr float kAlertFlashSeconds = 0.9f;

const char* kCrashDumpHelp =
    "Sends this session's crash dump after the report to help find the cause. A crash dump holds the program's threads and "
    "loaded modules at the moment of the crash, and may contain some of its memory.";

const char* kPrivacyHeadline = "No personal user data is intentionally included.";
const char* kPrivacyNote =
    "Before anything is sent, the launcher takes addresses, user and computer names, private folders, passwords, secrets and "
    "other private details out of the selected log. Automatic filtering cannot promise to catch everything, so open the "
    "preview if you want to check what goes out.";

int KeepDescriptionShort(ImGuiInputTextCallbackData* data) {
    const size_t keep = BytesOfCharacters(std::string_view(data->Buf, size_t(data->BufTextLen)), kMostDescriptionCharacters);
    if (keep < size_t(data->BufTextLen)) data->DeleteChars(int(keep), data->BufTextLen - int(keep));
    return 0;
}

std::string LogDetails(const GameLog& log) { return LogDateText(log.written) + kMiddleDot + LogSizeText(log.bytes); }

void LogStatusLine(trg::Ui& ui, const GameLog& log) {
    const char* role = LogRoleText(log.role);
    const char* ended = SessionEndText(log);
    if (!role && !ended) return;
    ImGui::PushFont(nullptr, trg::layout::kTextHelp);
    if (role) AccentText(ui, role);
    if (ended) {
        if (role) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.0f);
        ImGui::TextColored(ui.theme().warn_dot, "%s", Tr(ended));
    }
    ImGui::PopFont();
}

ImVec4 AlertedColor(const ImVec4& color, const ImVec4& alert, double seconds) {
    const float flash = std::max(0.0f, 1.0f - float(seconds) / kAlertFlashSeconds);
    const float t = kAlertTint + (kAlertFlashTint - kAlertTint) * flash;
    return ImVec4(color.x + (alert.x - color.x) * t, color.y + (alert.y - color.y) * t, color.z + (alert.z - color.z) * t, color.w);
}

float SemiboldWidth(trg::Ui& ui, const char* text) {
    ImGui::PushFont(ui.fonts().semibold, trg::layout::kTextLabel);
    const float width = ImGui::CalcTextSize(Tr(text)).x;
    ImGui::PopFont();
    return width;
}

void FactLabel(trg::Ui& ui, const char* text) {
    ImGui::PushFont(ui.fonts().semibold, trg::layout::kTextLabel);
    ImGui::TextUnformatted(Tr(text));
    ImGui::PopFont();
}

void FactValue(trg::Ui& ui, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().dim);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void DrawFactLine(trg::Ui& ui, float left, float right, float y) {
    const float top = std::floor(y);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(left, top), ImVec2(right, top + 1.0f), ImGui::GetColorU32(ui.theme().border_row));
}

// The launcher's two-pair grid (Ui::InfoGrid) with the button in its empty second-row cell, flush right like the page's other
// buttons. True when it was pressed.
bool DrawLogFacts(trg::Ui& ui, const GameLog& log) {
    const float scale = ui.scale();
    const float padding = trg::layout::kInfoGridPadding * scale;
    const std::string written = LogDateText(log.written);
    const std::string size = LogSizeText(log.bytes);
    const float left = ImGui::GetCursorScreenPos().x;
    const float room = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, padding));
    if (!ImGui::BeginTable("##selected_log", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoClip)) {
        ImGui::PopStyleVar();
        return false;
    }
    const float weights[4] = {std::max(SemiboldWidth(ui, "Selected log"), SemiboldWidth(ui, "Size")),
                              std::max(ImGui::CalcTextSize(log.name.c_str()).x, ImGui::CalcTextSize(size.c_str()).x),
                              SemiboldWidth(ui, "Last written"), ImGui::CalcTextSize(written.c_str()).x};
    for (const float weight : weights) ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch, std::max(weight, 1.0f));
    ImGui::TableNextColumn();
    FactLabel(ui, "Selected log");
    ImGui::TableNextColumn();
    FactValue(ui, log.name);
    ImGui::TableNextColumn();
    FactLabel(ui, "Last written");
    ImGui::TableNextColumn();
    FactValue(ui, written);

    ImGui::TableNextColumn();
    const float second_row = ImGui::GetCursorScreenPos().y - padding;
    ImGui::AlignTextToFramePadding();
    FactLabel(ui, "Size");
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    FactValue(ui, size);
    ImGui::TableNextColumn();
    const ImVec2 cell = ImGui::GetCursorScreenPos();
    const float width = std::min(trg::layout::ControlColumnWidth(room, scale), left + room - cell.x);
    ImGui::SetCursorScreenPos(ImVec2(left + room - width, cell.y));
    const bool pressed = ImGui::Button(Tr("View selected log"), ImVec2(width, 0));
    ImGui::EndTable();
    ImGui::PopStyleVar();
    DrawFactLine(ui, left, left + room, second_row);
    DrawFactLine(ui, left, left + room, ImGui::GetItemRectMax().y);
    return pressed;
}

std::string EnvironmentValue(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

} // namespace

IncidentPage::IncidentPage(Places places)
    : places_(std::move(places)), my_reports_(places_.user_dir / kReportHistoryFile) {
    service_.endpoint = ResolveEndpoint(kBugReportServiceUrl, EnvironmentValue(kBugReportUrlVariable));
    service_.cancel = std::make_shared<std::atomic<bool>>(false);
    service_.log = places_.log;
    Log(service_.endpoint.configured() ? "Bug report: service " + service_.endpoint.url : "Bug report: service address not valid");
}

IncidentPage::~IncidentPage() {
    if (service_.cancel) service_.cancel->store(true);
}

void IncidentPage::Draw(trg::Ui& ui, const ReportFacts& facts) {
    if (!logs_found_) FindLogs();
    CollectDelivery();
    CollectDumpRetry();
    my_reports_.RefreshIfStale(service_);
    const bool sending = SendBusy(State());
    if (CheckDraft(description_, !logs_.empty()) != problem_) problem_ = DraftProblem::kNone;
    KeepClearOfScrollbar(std::floor(trg::layout::kContentPadding * ui.scale()));
    AccentText(ui, kPrivacyHeadline);
    ui.Paragraph(kPrivacyNote);
    if (!service_.endpoint.configured()) WarningText(ui, service_.endpoint.problem.c_str());
    if (my_reports_.HasUpdates()) {
        const bool reply = my_reports_.HasNewReply();
        if (ui.ButtonRow(reply ? "A report has a new reply" : "A report has an update",
                         reply ? "The developer replied to one of your reports." : "Its status changed since you last looked.", "Show")) {
            my_reports_.ShowUpdate();
        }
    }
    ui.Heading("1. Report the problem");
    DrawDescription(ui, sending);
    ui.Heading("2. Choose one game log");
    DrawLogs(ui, sending);
    ui.Heading("3. Preview what will be sent");
    DrawPreview(ui, facts, sending);
    ui.Heading("4. Send");
    DrawSend(ui, facts, sending);
    ui.EndRows();
    my_reports_.Draw(ui, service_);
    ui.EndRows();
}

SendState IncidentPage::State() const {
    SendFacts in_flight;
    in_flight.delivering = delivery_ != nullptr;
    in_flight.uploading_dump = delivery_ && delivery_->uploading_dump;
    in_flight.retrying_dump = dump_retry_ != nullptr;
    in_flight.result = result_ ? &*result_ : nullptr;
    return ClassifySend(in_flight);
}

void IncidentPage::FindLogs() {
    logs_ = FindGameLogs(places_.logs_dir, places_.running_log);
    logs_found_ = true;
    SelectLog(DefaultGameLog(logs_));
}

// Choosing a log chooses its crash dump too: attached unless the player says otherwise, and only when it can be sent.
void IncidentPage::SelectLog(size_t index) {
    selected_ = index;
    attach_crash_dump_ = selected_ < logs_.size() && DefaultAttachCrashDump(logs_[selected_]);
}

void IncidentPage::DraftChanged() {
    ForgetPreview();
    submission_.clear();
}

void IncidentPage::ForgetPreview() {
    previewed_.reset();
    preview_text_.clear();
    log_failed_ = false;
}

bool IncidentPage::AcceptDraft() {
    problem_ = CheckDraft(description_, !logs_.empty());
    problem_time_ = ImGui::GetTime();
    bring_problem_into_view_ = problem_ != DraftProblem::kNone;
    return problem_ == DraftProblem::kNone;
}

void IncidentPage::CollectDelivery() {
    if (!delivery_ || !delivery_->done) return;
    result_ = delivery_->receipt;
    sent_dump_ = delivery_->package->crash_dump;
    if (result_->outcome == ReportOutcome::kSent) {
        Log("Bug report: sent as " + result_->ticket);
        sent_report_remembered_ = my_reports_.Remember(*delivery_->package, *result_);
        description_[0] = '\0';
        DraftChanged();
    } else {
        Log(std::string("Bug report: not sent (") + OutcomeTitle(result_->outcome) + ")");
    }
    delivery_.reset();
    bring_result_into_view_ = true;
}

void IncidentPage::CollectDumpRetry() {
    if (!dump_retry_ || !dump_retry_->done) return;
    if (result_) result_->dump = dump_retry_->outcome;
    dump_retry_.reset();
    bring_result_into_view_ = true;
}

// Only the dump goes again, under the Report ID the service already gave: the report itself is never filed twice.
void IncidentPage::RetryDump() {
    if (!result_ || !CanRetryDump(State(), sent_dump_.has_value(), !result_->credential.empty())) return;
    dump_retry_ = std::make_shared<DumpRetry>();
    bring_result_into_view_ = true;
    std::thread([retry = dump_retry_, service = service_, ticket = result_->ticket, receipt = result_->credential, dump = *sent_dump_] {
        retry->outcome = UploadCrashDump(service, ticket, receipt, dump);
        retry->done = true;
    }).detach();
}

std::shared_ptr<const IncidentPackage> IncidentPage::BuildPackage(const ReportFacts& facts) {
    log_failed_ = false;
    const PrivateFacts private_facts = GatherPrivateFacts(places_.install_folders);
    const GameLog& log = logs_[selected_];
    std::optional<IncidentPackage> package =
        BuildIncidentPackage(description_, log, facts, private_facts, attach_crash_dump_ && log.CanAttachDump());
    if (!package) {
        log_failed_ = true;
        bring_problem_into_view_ = true;
        FindLogs();
        return nullptr;
    }
    if (package->crash_dump_requested && !package->crash_dump) {
        Log(std::string("Bug report: crash dump not attached (") + DumpProblemText(package->crash_dump_problem) + ")");
    }
    return std::make_shared<const IncidentPackage>(std::move(*package));
}

void IncidentPage::TogglePreview(const ReportFacts& facts) {
    if (previewed_) {
        ForgetPreview();
        return;
    }
    if (!AcceptDraft()) return;
    previewed_ = BuildPackage(facts);
    if (previewed_) preview_text_ = IncidentPreviewText(*previewed_);
}

void IncidentPage::Send(const ReportFacts& facts) {
    if (!CanSend(State(), service_.endpoint.configured()) || !AcceptDraft()) return;
    std::shared_ptr<const IncidentPackage> package = previewed_ ? previewed_ : BuildPackage(facts);
    if (!package) return;
    if (submission_.empty()) submission_ = NewSubmissionKey();
    result_.reset();
    sent_dump_.reset();
    delivery_ = std::make_shared<Delivery>();
    delivery_->package = std::move(package);
    bring_result_into_view_ = true;
    std::thread([delivery = delivery_, service = service_, submission = submission_] {
        delivery->receipt = FileReport(service, *delivery->package, submission);
        const ReportReceipt& filed = delivery->receipt;
        if (filed.outcome == ReportOutcome::kSent && delivery->package->crash_dump) {
            delivery->uploading_dump = true;
            delivery->receipt.dump = filed.dump_expected
                                         ? UploadCrashDump(service, filed.ticket, filed.credential, *delivery->package->crash_dump)
                                         : DumpUpload::kFailed;
        }
        delivery->done = true;
    }).detach();
}

void IncidentPage::Save(const ReportFacts& facts) {
    if (!AcceptDraft()) return;
    std::shared_ptr<const IncidentPackage> package = previewed_ ? previewed_ : BuildPackage(facts);
    if (!package) return;
    saved_report_ = SaveIncidentReport(*package, places_.logs_dir / "reports");
    save_failed_ = saved_report_.empty();
    bring_saved_into_view_ = true;
    if (!save_failed_) {
        description_[0] = '\0';
        DraftChanged();
    }
}

void IncidentPage::DrawDescription(trg::Ui& ui, bool sending) {
    ui.EndRows();
    const bool too_short = problem_ == DraftProblem::kDescriptionTooShort;
    if (too_short && std::exchange(bring_problem_into_view_, false)) ImGui::SetKeyboardFocusHere();
    const ImVec4 frame = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    ImGui::BeginDisabled(sending);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, too_short ? AlertedColor(frame, ui.theme().warn_dot, ImGui::GetTime() - problem_time_) : frame);
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_CallbackEdit;
    if (InputTextMultilineKeepOnEscape("##description", description_, sizeof(description_),
                                       ImVec2(-FLT_MIN, kDescriptionHeight * ui.scale()), flags, KeepDescriptionShort)) {
        DraftChanged();
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    const size_t used = CountCharacters(description_);
    const std::string counter = std::to_string(used) + " / " + std::to_string(kMostDescriptionCharacters);
    ImGui::PushFont(nullptr, trg::layout::kTextHelp);
    ImGui::TextColored(used >= kMostDescriptionCharacters ? ui.theme().warn_dot : ui.theme().dim, "%s", counter.c_str());
    if (too_short) {
        ImGui::SameLine();
        ImGui::TextColored(ui.theme().warn_dot, "%s", Tr("Please describe the problem, minimum 10 characters"));
    }
    ImGui::PopFont();
    ui.Help("What were you doing, what went wrong, and does it happen every time?");
}

void IncidentPage::DrawLogs(trg::Ui& ui, bool sending) {
    if (logs_.empty()) {
        ui.Paragraph("No game log is available yet, so a report cannot be sent. Start the game once and try again. With "
                     "Detailed logging off on the About page, only sessions that crashed keep a log.");
        if (problem_ == DraftProblem::kNoLog) {
            if (std::exchange(bring_problem_into_view_, false)) ImGui::SetScrollHereY();
            WarningText(ui, "There is no game log to send.");
        }
        return;
    }
    ui.Help("The newest logs of the game, most recent first. Only the one you choose is sent.");
    for (size_t i = 0; i < logs_.size(); ++i) {
        const std::string details = LogDetails(logs_[i]);
        ui.RowLeft(logs_[i].name.c_str(), details.c_str());
        LogStatusLine(ui, logs_[i]);
        ui.RowControl();
        ImGui::PushID(int(i));
        ImGui::BeginDisabled(sending);
        ImGui::AlignTextToFramePadding();
        if (ImGui::RadioButton(Tr("Send this log"), selected_ == i) && selected_ != i) {
            SelectLog(i);
            DraftChanged();
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ui.EndRows();
    if (log_failed_) {
        if (std::exchange(bring_problem_into_view_, false)) ImGui::SetScrollHereY();
        WarningText(ui, "That log could not be read. Choose another one and try again.");
    }
    const GameLog& log = logs_[selected_];
    if (DrawLogFacts(ui, log)) trg::OpenUrl(Utf8(log.path));
    DrawCrashDump(ui, sending);
}

void IncidentPage::DrawCrashDump(trg::Ui& ui, bool sending) {
    const GameLog& log = logs_[selected_];
    if (!log.dump) return;
    const bool attachable = log.CanAttachDump();
    ui.Spacer(4);
    ImGui::BeginDisabled(sending || !attachable);
    bool attach = attach_crash_dump_ && attachable;
    if (ImGui::Checkbox(Tr("Attach crash dump"), &attach)) {
        attach_crash_dump_ = attach;
        DraftChanged();
    }
    ImGui::EndDisabled();
    if (attachable) {
        const std::string help = std::string(Tr(kCrashDumpHelp)) + " " + log.dump->name + kMiddleDot + LogSizeText(log.dump->bytes);
        ui.Help(help.c_str());
    } else {
        const std::string why = std::string(Tr(DumpProblemText(log.dump_problem))) + " " + Tr("Only the log is sent.");
        ui.Help(why.c_str());
    }
}

void IncidentPage::DrawPreview(trg::Ui& ui, const ReportFacts& facts, bool sending) {
    ImGui::BeginDisabled(sending);
    const bool clicked = ui.ButtonRow("Preview", "Shows exactly what will be sent, after the privacy filter.", PreviewButtonText(previewed_ != nullptr));
    ImGui::EndDisabled();
    if (clicked) TogglePreview(facts);
    ui.EndRows();
    if (previewed_) ReadOnlyBox("##preview", preview_text_, kPreviewHeight * ui.scale());
}

void IncidentPage::DrawSend(trg::Ui& ui, const ReportFacts& facts, bool sending) {
    ImGui::BeginDisabled(sending || !service_.endpoint.configured());
    const bool send = ui.ButtonRow("Send report", "Sends the selected log after the privacy filter is applied.",
                                   sending ? "Sending..." : "Send report", true);
    ImGui::EndDisabled();
    if (send) Send(facts);
    ui.EndRows();
    DrawResult(ui);
    if (std::exchange(bring_result_into_view_, false)) ImGui::SetScrollHereY(1.0f);
    DrawOtherOptions(ui, facts, sending);
}

void IncidentPage::DrawOtherOptions(trg::Ui& ui, const ReportFacts& facts, bool sending) {
    ui.Spacer(4);
    ImGui::BeginDisabled(sending);
    const bool save = ui.ButtonRow("Save a copy", "Writes the same filtered report to a text file in the logs folder.", "Save report");
    ImGui::EndDisabled();
    if (save) Save(facts);
    if (ui.ButtonRow("Logs folder", "The game logs, and the copies saved here.", "Open logs folder")) {
        std::error_code ec;
        std::filesystem::create_directories(places_.logs_dir, ec);
        trg::OpenInFileManager(Utf8(places_.logs_dir));
    }
    if (ui.ButtonRow("Report on GitHub", "Opens the issue tracker in your browser.", "Open GitHub issues")) trg::OpenUrl(kIssuesUrl);
    ui.EndRows();
    DrawSaved(ui);
    if (std::exchange(bring_saved_into_view_, false)) ImGui::SetScrollHereY(1.0f);
}

void IncidentPage::DrawResult(trg::Ui& ui) {
    const SendState state = State();
    if (state == SendState::kIdle) return;
    ui.Spacer(4);
    switch (state) {
    case SendState::kIdle: return;
    case SendState::kUploadingDump:
        ui.StatusCard(trg::Status::kBusy, "Uploading the crash dump...", "The report is already sent; the dump can take a little longer.");
        return;
    case SendState::kSending: ui.StatusCard(trg::Status::kBusy, "Sending your report...", "This takes a few seconds."); return;
    case SendState::kFailed: {
        std::string advice = Tr(OutcomeAdvice(result_->outcome));
        const std::string retry = RetryAfterText(result_->retry_after_seconds);
        if (!retry.empty()) advice += " " + std::string(Tr(retry));
        ui.StatusCard(trg::Status::kAttention, OutcomeTitle(result_->outcome), advice.c_str());
        return;
    }
    case SendState::kSent:
    case SendState::kSentDumpFailed: break;
    }
    const std::string id = std::string(Tr("Report ID:")) + " " + result_->ticket;
    ui.StatusCard(trg::Status::kReady, "Report sent successfully", id.c_str());
    ui.Spacer(4);
    if (ImGui::Button(Tr("Copy report ID"))) {
        ImGui::SetClipboardText(result_->ticket.c_str());
        ui.SetStatus("Report ID copied.");
    }
    DrawDumpResult(ui);
    if (!sent_report_remembered_) ui.Help("The report was sent, but it could not be added to My Reports on this PC.");
}

void IncidentPage::DrawDumpResult(trg::Ui& ui) {
    switch (result_->dump) {
    case DumpUpload::kNotRequested: break;
    case DumpUpload::kUploaded: ui.Help("The crash dump was uploaded with it."); break;
    case DumpUpload::kNotAttached: ui.Help("The crash dump could not be attached, so only the log was sent."); break;
    case DumpUpload::kFailed:
        WarningText(ui, "The report was sent, but its crash dump could not be uploaded.");
        if (CanRetryDump(State(), sent_dump_.has_value(), !result_->credential.empty()) &&
            ImGui::Button(Tr("Retry crash dump upload"))) {
            RetryDump();
        }
        break;
    }
}

void IncidentPage::DrawSaved(trg::Ui& ui) {
    if (save_failed_) {
        ui.Spacer(4);
        WarningText(ui, "The report could not be saved.");
    } else if (!saved_report_.empty()) {
        ui.Spacer(4);
        const std::string name = Utf8(saved_report_.filename());
        ui.StatusCard(trg::Status::kReady, "Report saved", name.c_str());
    }
}

} // namespace Sotb
