// A session that ended through the controlled device-lost exit (exit code 25) reaches the Bug Report as a GPU device
// loss, with the first owner's reason, and the GPU timeline stays in the log. The log lines are the device-lost monitor's
// own output (vk_device_lost.h) written into a synthetic session log; no Vulkan device.
#include <string>
#include <vector>

#include "crash_report_test_support.h"
#include "json_lite.h"
#include "video_core/renderer_vulkan/vk_device_lost.h"

using namespace crash_test;
using namespace Sotb;
namespace Lost = Vulkan::DeviceLost;

namespace {

std::vector<std::string> printed;
const std::string kGpuPrefix = "GPU [Render_Vulkan] <Critical> vk_instance.cpp:358 operator(): ";

Vulkan::FaultReport PageFault() {
    Vulkan::FaultReport fault;
    fault.status = Vulkan::FaultStatus::Captured;
    fault.description = "page fault in the graphics queue";
    fault.address_count = 1;
    fault.addresses.push_back({1, 0x1000, 0});
    return fault;
}

Lost::Hooks Hooks(Vulkan::FaultReport (*query)(void*)) {
    printed.clear();
    Lost::Hooks hooks;
    hooks.query_fault = query;
    hooks.print = [](const char* line) { printed.emplace_back(line); };
    return hooks;
}

// The log a device-lost session leaves: its start, the monitor's lines as the video core logs them, then what other
// threads wrote before the process ended.
std::string DeviceLostLog(const std::string& after = {}) {
    std::string log = kGameplayHead + "frame stats: 60 fps\n";
    for (const std::string& line : printed) log += kGpuPrefix + line + "\n";
    return log + after;
}

Lost::Monitor& LoseDevice(Lost::Monitor& monitor, Vulkan::FaultReport (*query)(void*)) {
    monitor.SetHooks(Hooks(query));
    monitor.SetDevice("Test GPU", "9.9");
    for (int tick = 1; tick <= 200; ++tick) monitor.Report(Lost::Operation::QueueSubmit, "submit", Lost::Success, uint64_t(tick));
    monitor.Report(Lost::Operation::QueueSubmit, "submit", Lost::DeviceLostResult, 201);
    return monitor;
}

Vulkan::FaultReport QueryPageFault(void*) { return PageFault(); }

// 1, 4 to 8, 15: the reason the first owner published, with its operation, result, stage, GPU and driver.
void TestDeviceLostSession(const fs::path& scratch) {
    Lost::Monitor monitor;
    LoseDevice(monitor, &QueryPageFault);
    CHECK(monitor.HasPublishedIncident());
    const std::string reason = monitor.Reason();
    const GameLog log = Found(MakeLog(scratch / "lost", 12, DeviceLostLog()));
    CHECK(log.outcome.end == SessionEnd::kDeviceLost && SessionEndText(log) == std::string("GPU device lost"));
    CHECK(log.outcome.detail == reason && log.outcome.stage == "first frame presented");
    CHECK(!log.Crashed() && !log.dump);

    const auto package = BuildIncidentPackage("The screen went black and the game closed", log, Facts(), Private(), true);
    CHECK(package.has_value());
    if (!package) return;
    const std::string detail = package->Value("session_detail");
    CHECK(package->Value("session_end") == "device_lost" && detail == reason);
    CHECK(Has(detail, "vkQueueSubmit") && Has(detail, "(submit)") && Has(detail, "result -4") && Has(detail, "tick 201"));
    CHECK(Has(detail, "GPU Test GPU") && Has(detail, "driver 9.9") && Has(detail, "device fault: page fault in the graphics queue"));
    CHECK(package->Value("session_stage") == "first frame presented");
    const auto request = json::Parse(IncidentRequestJson(*package));
    CHECK(request && request->StringOr("session_end") == "device_lost" && request->StringOr("session_detail") == reason);
}

// 10: the timeline is the log's; no other field repeats it.
void TestTimelineStaysInTheLog(const fs::path& scratch) {
    Lost::Monitor monitor;
    LoseDevice(monitor, &QueryPageFault);
    const GameLog log = Found(MakeLog(scratch / "timeline", 13, DeviceLostLog()));
    const auto package = BuildIncidentPackage("The game froze and closed itself", log, Facts(), Private(), false);
    CHECK(package && Has(package->Value("log_text"), "GPU timeline: latest 128 of 201 GPU operations before the loss"));
    CHECK(package && Has(package->Value("log_text"), "DEVICE LOST"));
    for (const IncidentField& field : package->fields) {
        if (field.key != "log_text") CHECK(!Has(field.value, "GPU timeline"));
    }
    CHECK(Lost::TimelineCapacity == 128);
}

// 9, 11: a long driver text stays bounded; without fault data the reason says so, and a log cut short still names the loss.
void TestBoundsAndMissingData(const fs::path& scratch) {
    Lost::Monitor bare;
    LoseDevice(bare, nullptr);
    const GameLog plain = Found(MakeLog(scratch / "bare", 14, DeviceLostLog()));
    CHECK(plain.outcome.end == SessionEnd::kDeviceLost && Has(plain.outcome.detail, "device fault unavailable"));

    Lost::Monitor wordy;
    LoseDevice(wordy, [](void*) {
        Vulkan::FaultReport fault = PageFault();
        fault.description = std::string(Vulkan::MaxFaultTextBytes, 'w');
        return fault;
    });
    const GameLog long_log = Found(MakeLog(scratch / "wordy", 15, DeviceLostLog()));
    const auto package = BuildIncidentPackage("The game froze and closed itself", long_log, Facts(), Private(), false);
    CHECK(package && CountCharacters(package->Value("session_detail")) <= kMostSessionDetailCharacters);
    CHECK(package && package->Value("session_detail").starts_with("GPU device lost: vkQueueSubmit (submit) result -4"));
    CHECK(long_log.outcome.detail.size() <= Lost::MaxReasonBytes);

    const std::string cut = kGameplayHead + kGpuPrefix + "GPU device lost: first detected in vkWaitSemaphores (semwait)\n";
    const SessionOutcome first_only = ReadSessionOutcome(cut, cut);
    CHECK(first_only.end == SessionEnd::kDeviceLost && first_only.detail == "GPU device lost: first detected in vkWaitSemaphores (semwait)");
    CHECK(ReadSessionOutcome("", "").end == SessionEnd::kUnknown);
}

// 2, 3, 14: a crash stays a crash, a normal end is no failure, and the first fault owns the session.
void TestFirstFaultOwnsTheSession(const fs::path& scratch) {
    Lost::Monitor monitor;
    LoseDevice(monitor, &QueryPageFault);
    const std::string first = monitor.Reason();
    monitor.Report(Lost::Operation::QueuePresent, "present", Lost::DeviceLostResult, 300);
    CHECK(monitor.Reason() == first && monitor.Repeats() == 1);
    const std::string after = "STOP: GPU library assertion after the device was lost (a consequence of the first report)\n"
                              "Fault: read access to 0000000000000000 at guest offset 0x40 (thread 77)\n"
                              "FATAL exception 0xc0000005 at 0000000800000040 (guest offset 0x40) thread 77\n";
    const GameLog log = Found(MakeLog(scratch / "first", 16, DeviceLostLog(after)));
    CHECK(log.outcome.end == SessionEnd::kDeviceLost && log.outcome.detail == first);
    CHECK(SessionEndText(log) == std::string("GPU device lost"));

    const std::string crash_first = kGameplayHead + CrashTail(17) + kGpuPrefix + "GPU device lost: first detected in vkQueueSubmit (submit)\n";
    const GameLog crashed = Found(MakeLog(scratch / "first", 17, crash_first));
    CHECK(crashed.outcome.end == SessionEnd::kCrash && SessionEndText(crashed) == std::string("Crash detected"));
    CHECK(Has(crashed.outcome.detail, "FATAL exception 0xc0000005"));

    const GameLog clean = Found(MakeLog(scratch / "first", 18, kGameplayHead + kCleanTail));
    CHECK(clean.outcome.end == SessionEnd::kClean && !clean.Crashed() && !SessionEndText(clean));
    const std::string assertion = kGameplayHead + "STOP: GPU library assertion failed (see GPU log above)\n";
    CHECK(ReadSessionOutcome(assertion, assertion).end == SessionEnd::kStopped);
}

// 12, 13: the report goes with or without a dump of the same session.
void TestWithAndWithoutDump(const fs::path& scratch) {
    Lost::Monitor monitor;
    LoseDevice(monitor, &QueryPageFault);
    const fs::path folder = scratch / "dumps";
    const GameLog without = Found(MakeLog(folder, 19, DeviceLostLog()));
    const auto alone = BuildIncidentPackage("The game froze and closed itself", without, Facts(), Private(), true);
    CHECK(alone && !alone->crash_dump && alone->crash_dump_problem == DumpProblem::kMissing && alone->Value("session_end") == "device_lost");

    const std::string after = "Fault: read access to 0000000000000000 at guest offset 0x40 (thread 77)\nCrash: minidump " + DumpName(20) +
                              " written\nFATAL exception 0xc0000005 at 0000000800000040 (guest offset 0x40) thread 77\n";
    MakeLog(folder, 20, DeviceLostLog(after));
    MakeDump(folder, 20, DumpBytes(8192));
    const GameLog with = Found(folder / LogName(20));
    CHECK(with.outcome.end == SessionEnd::kDeviceLost && with.CanAttachDump());
    const auto both = BuildIncidentPackage("The game froze and closed itself", with, Facts(), Private(), true);
    CHECK(both && both->crash_dump && both->crash_dump->name == DumpName(20) && both->Value("session_end") == "device_lost");
}

// The exit code and the line the launcher keys on come from the one device-lost owner; neither is changed here.
void TestExitPathWiring() {
    CHECK(Lost::ExitStatus == 25);
    const std::string header = Source("gpu/shadps4/video_core/renderer_vulkan/vk_device_lost.h");
    const size_t own = header.find("void Own(");
    CHECK(own != std::string::npos);
    const std::string body = header.substr(own, header.find("std::atomic<Phase> phase", own) - own);
    const size_t first = body.find("\"GPU device lost: first detected in \"");
    CHECK(first != std::string::npos && first < body.find("PrintTimeline(snapshot);") && body.find("PrintTimeline(snapshot);") < body.find("Print(snapshot, incident.reason);"));
    CHECK(body.find("Print(snapshot, incident.reason);") < body.find("snapshot.request_stop(incident.reason.c_str());"));
    CHECK(Has(header, "\"GPU device lost: %s (%s) result %d, tick %llu\""));
    const std::string instance = Source("gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp");
    CHECK(Has(instance, "std::_Exit(DeviceLost::ExitStatus)") && Has(instance, ".request_stop = &StopAfterDeviceLoss"));
    CHECK(Has(instance, ".print = [](const char* line) { LOG_CRITICAL(Render_Vulkan, \"{}\", line); }"));
    const std::string assert_cpp = Source("gpu/shim/common/assert.cpp");
    CHECK(Has(assert_cpp, "STOP: GPU library assertion after the device was lost") && Has(assert_cpp, "std::_Exit(ExitStatus)"));
    const std::string logging = Source("gpu/shim/common/logging/log.cpp");
    CHECK(Has(logging, "\"GPU [%.*s] <%s> %s:%d %s: %s\\n\""));
    CHECK(Has(Source("launcher/incident_report.cpp"), "\"GPU device lost: first detected in \""));
}

} // namespace

int main() {
    const fs::path scratch = Scratch("sotb-device-lost-reporting-tests");
    TestDeviceLostSession(scratch);
    TestTimelineStaysInTheLog(scratch);
    TestBoundsAndMissingData(scratch);
    TestFirstFaultOwnsTheSession(scratch);
    TestWithAndWithoutDump(scratch);
    TestExitPathWiring();
    return Finish("device lost reporting", scratch);
}
