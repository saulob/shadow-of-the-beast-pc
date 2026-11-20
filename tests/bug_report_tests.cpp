// The Bug Report page's work without a window or a network: JSON, the service address, log choice, the privacy filter, the
// report and its preview, the client against a stand-in transport, My Reports, and the source checks that nothing is sent
// or listened for on its own. The service itself is tested in server/shadow-bug-reports.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "game_profile.h"
#include "http_post.h"
#include "incident_client.h"
#include "incident_report.h"
#include "json_lite.h"
#include "log_scrubber.h"
#include "report_history.h"

namespace fs = std::filesystem;
using namespace Sotb;

namespace {

int checks = 0;
int failures = 0;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(condition)) {                                                                                            \
            ++failures;                                                                                                \
            std::printf("FAIL line %d: %s\n", __LINE__, #condition);                                                   \
        }                                                                                                              \
    } while (0)

std::string Read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void Write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
}

bool Has(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

const std::string kHead =
    "[run] 2026-11-19T21:02:57\n"
    "CPU: Intel(R) Core(TM) i9-14900\n"
    "Stage: launcher handoff\n"
    "Launcher: closed, starting the game\n"
    "Build fingerprint: CUSA03762 1.00 Korea, region Korea, title CUSA03762, version 1.00, code CUSA03762-1.00\n"
    "Settings: Renderer auto, graphics card NVIDIA GeForce RTX 4070, VSync off, presentation letterbox\n"
    "Settings: Render resolution 1440p 2560x1440\n"
    "Renderer: selected auto, active Vulkan\n"
    "GPU: selected NVIDIA GeForce RTX 4070\n"
    "GPU: Vulkan device NVIDIA GeForce RTX 4070 (NVIDIA 621.1.0.0, Vulkan 1.4.357)\n"
    "Runtime: guest thread 'main' created (stack=1, prio=1)\n";

const std::string kPrivateLines =
    "Settings: C:\\Users\\ana.silva\\Documents\\shadow\\user/sotb.ini\n"
    "Modules: external module loaded: C:\\ProgramData\\obs-studio-hook\\graphics-hook64.dll at 00007FFF2A890000\n"
    "Modules: external module loaded: D:\\Games\\AnaStuff\\overlay64.dll at 00007FFF2A890000\n"
    "Host DESKTOP-ANA7 of user ana.silva started\n"
    "Share \\\\fileserver\\games\\shadow\\save.dat opened\n"
    "Config /home/ana.silva/.config/shadow/sotb.ini\n"
    "Remote 203.0.113.42:443 and 2001:db8:85a3::8a2e:370:7334 answered\n"
    "peer 123456789 joined\n"
    "token=abcdef0123456789abcdef0123\n"
    "password = hunter2\n"
    "Audio: ATRAC9 48000 Hz, driver version 10.0.26200.1\n";

PrivateFacts Facts() {
    PrivateFacts facts;
    facts.user_names = {"ana.silva"};
    facts.machine_names = {"DESKTOP-ANA7"};
    facts.profile_folders = {"C:\\Users\\ana.silva"};
    return facts;
}

ReportFacts Report() {
    return {"0.1.0", "CUSA03762 1.00 Korea", "1.00", "Korea", "CUSA03762", std::string(64, 'a'), "Windows 11 (build 26200)"};
}

fs::path MakeLog(const fs::path& folder, int number, const std::string& text, int minutes_ago) {
    char name[64];
    std::snprintf(name, sizeof(name), "shadow_of_the_beast_%03d.log", number);
    const fs::path file = folder / name;
    Write(file, text);
    fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::minutes(minutes_ago));
    return file;
}

GameLog LogOf(const fs::path& file) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(file, ec);
    return {file, file.filename().string(), 0, ec ? 0 : size, LogActivity::kGameplay, LogRole::kNone};
}

void TestJson() {
    CHECK(json::Quote("a\"b\\c\n\t\x01") == "\"a\\\"b\\\\c\\n\\t\\u0001\"");
    CHECK(json::Quote("caf\xC3\xA9") == "\"caf\xC3\xA9\"");
    CHECK(json::Quote("bad\xC3(") == "\"bad\\ufffd(\"");
    const auto parsed = json::Parse(R"({"a":"x\u00e9\n","n":-12,"t":true,"f":false,"z":null,"list":[{"k":"v"},[]]})");
    CHECK(parsed && parsed->IsObject());
    CHECK(parsed->StringOr("a") == "x\xC3\xA9\n" && parsed->NumberOr("n") == -12 && parsed->BoolOr("t") && !parsed->BoolOr("f"));
    CHECK(parsed->Find("list")->array[0].StringOr("k") == "v");
    CHECK(parsed->StringOr("n").empty() && parsed->NumberOr("a") == 0 && parsed->Find("missing") == nullptr);
    CHECK(json::Parse("\"\\ud83d\\ude00\"")->string == "\xF0\x9F\x98\x80");
    for (const char* bad : {"", "{", "{\"a\":}", "[1,]", "{\"a\":1} x", "1.5", "01", "\"\\ud83d\"", "\"\\x\"", "tru", "{'a':1}", "\"a\nb\"",
                            "99999999999999999999"}) {
        CHECK(!json::Parse(bad));
    }
    CHECK(!json::Parse(std::string(40, '[') + std::string(40, ']')));
    CHECK(json::Parse("  [1, 2 ]  ")->array.size() == 2);
    const std::string hostile = "line\n\"quote\" \\ \xE2\x82\xAC \x7F";
    CHECK(json::Parse(json::Quote(hostile)) && json::Parse(json::Quote(hostile))->string == "line\n\"quote\" \\ \xE2\x82\xAC \x7F");
}

void TestEndpoint() {
    CHECK(!ResolveEndpoint("", "").configured() && !ResolveEndpoint("", "").problem.empty());
    CHECK(ResolveEndpoint("", "").problem == "No bug report service is set up in this version yet.");
    CHECK(ResolveEndpoint(kBugReportServiceUrl, "https://dev.example:8443").url == "https://dev.example:8443");
    CHECK(ResolveEndpoint("https://built-in.example", "  ").url == "https://built-in.example");
    CHECK(!ResolveEndpoint(kBugReportServiceUrl, "http://dev.example").configured());
    CHECK(!ResolveEndpoint("https://built-in.example", "http://dev.example").configured());
    CHECK(std::string(kBugReportUrlVariable) == "SOTB_BUG_REPORT_URL");
    CHECK(ResolveEndpoint("https://reports.example.workers.dev", "").url == "https://reports.example.workers.dev");
    CHECK(ResolveEndpoint("https://reports.example/", "").url == "https://reports.example");
    CHECK(ResolveEndpoint("https://reports.example:8443", "").url == "https://reports.example:8443");
    CHECK(ResolveEndpoint("https://reports.example:443", "").url == "https://reports.example");
    CHECK(ResolveEndpoint("https://a.example", "https://b.example").url == "https://b.example");
    for (const char* bad : {"http://reports.example", "http://127.0.0.1:8792", "http://localhost:8792", "http://[::1]:8792", "https://localhost:8792/x", "ftp://reports.example", "reports.example", "https://",
                            "https://user@reports.example", "https://reports.example/v1", "https://reports.example?x=1",
                            "https://reports.example#x", "https://reports.example:99999", "https://rep orts.example", "https://-bad.example",
                            "http://127.0.0.1.evil.example", "https://[evil]/", "javascript:alert(1)", "https://reports.example\\x"}) {
        CHECK(!ResolveEndpoint(bad, "").configured());
        CHECK(!ResolveEndpoint("https://ok.example", bad).configured());
    }
    const std::string built_in = kBugReportServiceUrl;
    CHECK(built_in.starts_with("https://") && !Has(built_in, "localhost") && !Has(built_in, "127.") && built_in.back() != '/');
    for (const char* other_project : {"odin", "13-sentinels", "ssf2t", "dcp", "dragon", "alienation"}) CHECK(!Has(built_in, other_project));
}

std::string Source(const char* relative);
size_t Count(const std::string& text, std::string_view part);

void TestDefaultEndpoint() {
    ServiceAddress address;
    const std::string production = "https://shadow-bug-reports.saulobenigno.workers.dev";
    CHECK(production == kBugReportServiceUrl);
    const ServiceEndpoint normal = ResolveEndpoint(kBugReportServiceUrl, "");
    CHECK(normal.configured() && normal.url == production && normal.problem.empty());
    CHECK(ResolveEndpoint(kBugReportServiceUrl, "   ").url == production);
    const ServiceEndpoint override_endpoint = ResolveEndpoint(kBugReportServiceUrl, "https://test.example:8443");
    CHECK(override_endpoint.url == "https://test.example:8443" && override_endpoint.url != production);
    const ServiceEndpoint invalid = ResolveEndpoint(kBugReportServiceUrl, "http://test.example");
    CHECK(!invalid.configured() && invalid.url.empty() && !invalid.problem.empty());
    CHECK(std::string(kBugReportServiceUrl).starts_with("https://") && ParseServiceAddress(production, address));

    const std::string page = Source("launcher/incident_page.cpp");
    CHECK(Has(page, "ImGui::BeginDisabled(sending || !service_.endpoint.configured());"));
    const size_t preview_at = page.find("void IncidentPage::TogglePreview");
    const size_t send_at = page.find("void IncidentPage::Send(");
    CHECK(preview_at != std::string::npos && send_at > preview_at);
    const std::string preview = page.substr(preview_at, send_at - preview_at);
    CHECK(!Has(preview, "FileReport") && !Has(preview, "std::thread") && !Has(preview, "delivery_"));
    CHECK(Count(page, "std::thread") == 1 && !Has(page, "/health"));
    CHECK(!Has(Source("launcher/incident_client.cpp"), "/health") && !Has(Source("launcher/my_reports_page.cpp"), "/health"));

    for (const char* name : {"launcher/incident_page.cpp", "launcher/incident_client.cpp", "launcher/incident_client.h", "launcher/game_profile.h",
                             "launcher/my_reports_page.cpp", "launcher/http_post.cpp", "launcher/launcher.cpp"}) {
        CHECK(!Has(Source(name), "ADMIN_TOKEN") && !Has(Source(name), "Bearer") && !Has(Source(name), "/admin"));
    }
    const std::string launcher = Source("launcher/launcher.cpp");
    CHECK(!Has(launcher, "kBugReportServiceUrl") && !Has(launcher, "FileReport") && Count(launcher, "CheckReports()") == 1);
    const std::string wire = IncidentRequestJson(IncidentPackage{});
    CHECK(!Has(wire, "ADMIN_TOKEN") && !Has(wire, "Bearer"));
}

void TestLogChoice(const fs::path& scratch) {
    const fs::path folder = scratch / "logs";
    const std::string gameplay = kHead;
    const std::string started = "[run]\nLauncher: closed, starting the game\nBuild fingerprint: CUSA03762 1.00 Korea, region Korea\n";
    const std::string launcher_only = "[run]\nLauncher: shown\nLauncher: closed\n";
    const std::string crashed_launcher = "[run]\nLauncher: shown\n";
    const fs::path running = MakeLog(folder, 30, crashed_launcher, 0);
    MakeLog(folder, 29, gameplay, 10);
    MakeLog(folder, 28, launcher_only, 20);
    MakeLog(folder, 27, started, 30);
    MakeLog(folder, 26, gameplay, 40);
    MakeLog(folder, 25, crashed_launcher, 50);
    MakeLog(folder, 24, gameplay, 60);
    MakeLog(folder, 23, "", 70);
    Write(folder / "notes.txt", "x");
    Write(folder / "shadow_of_the_beast_x1.log", "[run]\n");
    Write(folder / "other_game_031.log", gameplay);
    Write(folder / "shadow_of_the_beast_12.log", gameplay);

    const std::vector<GameLog> logs = FindGameLogs(folder, running);
    CHECK(logs.size() == kMostGameLogs && kMostGameLogs == 5);
    CHECK(logs[0].name == "shadow_of_the_beast_030.log" && logs[0].role == LogRole::kCurrentLauncher);
    CHECK(logs[1].name == "shadow_of_the_beast_029.log" && logs[1].activity == LogActivity::kGameplay && logs[1].role == LogRole::kLastGameplay);
    CHECK(logs[2].name == "shadow_of_the_beast_027.log" && logs[2].activity == LogActivity::kGameStarted);
    CHECK(logs[3].name == "shadow_of_the_beast_026.log" && logs[3].role == LogRole::kOtherGameplay);
    CHECK(logs[4].name == "shadow_of_the_beast_025.log" && logs[4].role == LogRole::kOtherLauncher);
    for (const GameLog& log : logs) CHECK(log.name != "shadow_of_the_beast_028.log" && log.name != "shadow_of_the_beast_023.log" && log.bytes > 0);
    CHECK(DefaultGameLog(logs) == 1);
    CHECK(DefaultGameLog({}) == 0);
    CHECK(FindGameLogs(folder, running, 2).size() == 2);
    CHECK(FindGameLogs(scratch / "missing", running).empty());

    std::vector<GameLog> only_launcher(logs.begin(), logs.begin() + 1);
    CHECK(DefaultGameLog(only_launcher) == 0);
    std::vector<GameLog> mixed = {logs[0], logs[2], logs[1]};
    CHECK(DefaultGameLog(mixed) == 2);

    CHECK(CheckDraft("short", true) == DraftProblem::kDescriptionTooShort);
    CHECK(CheckDraft("          a bit        ", true) == DraftProblem::kDescriptionTooShort);
    CHECK(CheckDraft("long enough description", false) == DraftProblem::kNoLog);
    CHECK(CheckDraft("long enough description", true) == DraftProblem::kNone);
    CHECK(CheckDraft("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", true) == DraftProblem::kNone);
    CHECK(BytesOfCharacters("\xE2\x82\xAC\xE2\x82\xAC\xE2\x82\xAC", 2) == 6 && CountCharacters("\xE2\x82\xAC\xE2\x82\xAC") == 2);
}

void TestPrivacyFilter() {
    const std::string scrubbed = ScrubText(kHead + kPrivateLines, Facts());
    for (const char* secret : {"ana.silva", "DESKTOP-ANA7", "fileserver", "203.0.113.42", "2001:db8", "8a2e:370", "123456789",
                               "abcdef0123456789abcdef0123", "hunter2", "C:\\Users", "AnaStuff", "/home/"}) {
        if (Has(scrubbed, secret)) std::printf("  leaked: %s\n", secret);
        CHECK(!Has(scrubbed, secret));
    }
    for (const char* kept : {"Intel(R) Core(TM) i9-14900", "GPU: selected NVIDIA GeForce RTX 4070", "NVIDIA GeForce RTX 4070",
                             "NVIDIA 621.1.0.0, Vulkan 1.4.357", "Render resolution 1440p 2560x1440", "Build fingerprint: CUSA03762 1.00 Korea, region Korea",
                             "driver version 10.0.26200.1", "graphics-hook64.dll", "overlay64.dll", "Runtime: guest thread 'main' created"}) {
        if (!Has(scrubbed, kept)) std::printf("  lost: %s\n", kept);
        CHECK(Has(scrubbed, kept));
    }
    const std::string controls = ScrubText(std::string("a\r\nb\x01\x7F\tc\xC2\x85 d\xC3(\n"), PrivateFacts{});
    CHECK(controls == "a\nb\tc d?(\n");

    CHECK(ScrubText("mail me at jane.doe+sotb@mail.example.org. thanks", PrivateFacts{}) == "mail me at <email>. thanks");
    CHECK(ScrubText("a@b.co and c@d.io", PrivateFacts{}) == "<email> and <email>");
    CHECK(ScrubText("shader main@4 and x@y.1 and @handle", PrivateFacts{}) == "shader main@4 and x@y.1 and @handle");

    const std::string linux_paths = ScrubText(
        "Runtime: game /home/ana.silva/Games/SOTB/game/prepared/CUSA03762-1.00/eboot.bin\n"
        "Launcher: AppImage /var/home/anasilva/Apps/ShadowOfTheBeastPC-v0.1.0-x86_64.AppImage\n"
        "Launcher: settings /run/media/anasilva/SD/SOTB/user/sotb.ini\n"
        "Launcher: package /mnt/c/Users/AnaStuff/Downloads/shadow.pkg\n"
        "Launcher: program /tmp/.mount_ShadowQx7Lm2/usr/bin/shadow_of_the_beast\n",
        PrivateFacts{});
    for (const char* secret : {"ana.silva", "anasilva", "AnaStuff", "/mnt/c/Users", "/home/", "/var/home/", "/run/media/"}) {
        if (Has(linux_paths, secret)) std::printf("  leaked: %s\n", secret);
        CHECK(!Has(linux_paths, secret));
    }
    CHECK(Has(linux_paths, "<home>/Games/SOTB/game/prepared/CUSA03762-1.00/eboot.bin"));
    for (const char* kept : {"eboot.bin", "ShadowOfTheBeastPC-v0.1.0-x86_64.AppImage", "sotb.ini", "shadow.pkg",
                             "/tmp/.mount_ShadowQx7Lm2/usr/bin/shadow_of_the_beast"}) {
        if (!Has(linux_paths, kept)) std::printf("  lost: %s\n", kept);
        CHECK(Has(linux_paths, kept));
    }

    const auto generic = [](std::string name) {
        for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
        for (const char* word : {"admin", "administrator", "deck", "default", "game", "guest", "localhost", "owner", "player",
                                 "public", "root", "steam", "steamdeck", "user"}) {
            if (name == word) return true;
        }
        return false;
    };
    PrivateFacts real = GatherPrivateFacts({});
    for (const std::string& name : real.user_names) {
        if (name.size() < 4 || generic(name)) continue;
        CHECK(!Has(ScrubText("Hello " + name + " here\nC:\\Users\\" + name + "\\x\n", real), name));
    }
    for (const std::string& name : real.machine_names) {
        if (name.size() < 4 || generic(name)) continue;
        CHECK(!Has(ScrubText("Machine " + name + " here\n", real), name));
    }
}

void TestDiagnostics() {
    const LogDiagnostics vulkan = ReadLogDiagnostics(kHead);
    CHECK(vulkan.cpu == "Intel(R) Core(TM) i9-14900");
    CHECK(vulkan.renderer_selected == "auto" && vulkan.renderer == "Vulkan" && vulkan.gpu == "NVIDIA GeForce RTX 4070");
    CHECK(vulkan.driver == "NVIDIA 621.1.0.0, Vulkan 1.4.357" && vulkan.render_resolution == "1440p 2560x1440");
    const LogDiagnostics opengl = ReadLogDiagnostics(
        "CPU: AMD Ryzen 7\nSettings: Renderer opengl, graphics card Radeon RX 7800, VSync on\nSettings: Render resolution 720p 1280x720\n"
        "Renderer: selected opengl, active OpenGL\n");
    CHECK(opengl.renderer_selected == "opengl" && opengl.renderer == "OpenGL" && opengl.gpu == "Radeon RX 7800" && opengl.driver.empty());
    const LogDiagnostics fallback = ReadLogDiagnostics("Renderer: selected auto, active OpenGL\nRenderer: Auto fell back to OpenGL: no Vulkan\n");
    CHECK(fallback.renderer_selected == "auto" && fallback.renderer == "OpenGL");
    const LogDiagnostics none = ReadLogDiagnostics("nothing here\n");
    CHECK(none.cpu.empty() && none.renderer_selected.empty() && none.renderer.empty() && none.gpu.empty() && none.driver.empty() &&
          none.render_resolution.empty());
    CHECK(ReadLogDiagnostics("indented GPU: Vulkan device X (Y)\n").gpu.empty());
    CHECK(Clamped("  a\tb\x01 c  ", 64) == "a b  c" && Clamped(std::string(200, 'x'), 64).size() == 64);
    CHECK(Clamped("\xE2\x82\xAC\xE2\x82\xAC\xE2\x82\xAC", 2) == "\xE2\x82\xAC\xE2\x82\xAC" && Clamped("   ", 5).empty());
}

// The rules the service holds a request to (server/shadow-bug-reports/src/reports.js).
void CheckAgainstServiceRules(const IncidentPackage& package) {
    const auto chars = [](const std::string& text) { return CountCharacters(text); };
    const auto matches = [](const std::string& value, const char* pattern) { return std::regex_match(value, std::regex(pattern)); };
    const auto clean = [](const std::string& value) {
        return (value.empty() || (value.front() != ' ' && value.back() != ' ')) &&
               std::none_of(value.begin(), value.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; });
    };
    CHECK(chars(package.Value("description")) >= 10 && chars(package.Value("description")) <= 500);
    CHECK(matches(package.Value("launcher_version"), "[0-9A-Za-z][0-9A-Za-z.+-]{0,31}"));
    CHECK(chars(package.Value("region")) <= 64);
    CHECK(!package.Value("game_build").empty() && chars(package.Value("game_build")) <= 64);
    CHECK(package.Value("game_version").empty() || matches(package.Value("game_version"), "[0-9][0-9.]{0,15}"));
    CHECK(package.Value("cusa").empty() || matches(package.Value("cusa"), "CUSA[0-9]{5}"));
    CHECK(package.Value("eboot_sha256").empty() || matches(package.Value("eboot_sha256"), "[0-9a-f]{64}"));
    CHECK(!package.Value("operating_system").empty() && chars(package.Value("operating_system")) <= 64);
    CHECK(chars(package.Value("cpu")) <= 96 && chars(package.Value("renderer_selected")) <= 64);
    CHECK(chars(package.Value("renderer")) <= 64 && chars(package.Value("gpu")) <= 96 && chars(package.Value("driver")) <= 96);
    CHECK(chars(package.Value("render_resolution")) <= 64);
    CHECK(matches(package.Value("log_file"), "shadow_of_the_beast_[0-9]{3,10}\\.log"));
    CHECK(package.Value("log_clipped") == "yes" || package.Value("log_clipped") == "no");
    CHECK(package.Value("log_text").size() <= kMostLogBytes && !package.Value("log_text").empty());
    CHECK(package.Value("log_text").find('\r') == std::string::npos);
    for (const char* key : {"launcher_version", "game_build", "game_version", "region", "cusa", "eboot_sha256", "operating_system", "cpu",
                            "renderer_selected", "renderer", "gpu", "driver", "render_resolution", "log_file", "log_clipped"}) {
        CHECK(clean(package.Value(key)));
    }
}

void TestReport(const fs::path& scratch) {
    const fs::path folder = scratch / "report";
    const fs::path file = MakeLog(folder, 12, kHead + kPrivateLines + "frame 1\nframe 2\n", 5);
    const GameLog log = LogOf(file);
    const std::string description = "  The game froze at the title screen for user ana.silva on DESKTOP-ANA7  ";

    const auto built = BuildIncidentPackage(description, log, Report(), Facts());
    CHECK(built.has_value());
    const IncidentPackage& package = *built;
    CHECK(package.Value("description") == "The game froze at the title screen for user <user> on <computer>");
    CHECK(package.Value("launcher_version") == "0.1.0" && package.Value("game_version") == "1.00" && package.Value("cusa") == "CUSA03762");
    CHECK(package.Value("region") == "Korea" && package.Value("game_build") == "CUSA03762 1.00 Korea");
    CHECK(package.Value("cpu") == "Intel(R) Core(TM) i9-14900" && package.Value("renderer_selected") == "auto");
    CHECK(package.Value("renderer") == "Vulkan" && package.Value("gpu") == "NVIDIA GeForce RTX 4070" && package.Value("render_resolution") == "1440p 2560x1440");
    CHECK(package.Value("log_file") == "shadow_of_the_beast_012.log" && package.Value("log_clipped") == "no");
    CHECK(!Has(package.Value("log_text"), "ana.silva") && !Has(package.Value("log_text"), "hunter2"));
    CheckAgainstServiceRules(package);

    // What the player previews is the package; what is sent is the same package.
    const std::string preview = IncidentPreviewText(package);
    const std::string request = IncidentRequestJson(package);
    const auto sent = json::Parse(request);
    CHECK(sent && sent->IsObject() && sent->object.size() == package.fields.size());
    for (const IncidentField& field : package.fields) {
        CHECK(sent->StringOr(field.key) == field.value);
        CHECK(Has(preview, field.value.empty() ? "-" : field.value));
    }
    CHECK(!Has(request, "ana.silva") && !Has(request, "DESKTOP-ANA7") && !Has(preview, "ana.silva"));
    CHECK(IncidentRequestJson(package) == request && IncidentPreviewText(package) == preview);
    CHECK(SentReportOf(*sent).Value("log_text") == package.Value("log_text") && SentReportOf(*sent).Value("gpu") == package.Value("gpu"));

    const fs::path out = fs::current_path() / "bug-report-request.json";
    Write(out, request);

    // The keys are the canonical request's, so the client and the service cannot drift apart.
    const fs::path contract = fs::path(SOTB_SOURCE_DIR) / "server" / "shadow-bug-reports" / "test" / "fixtures" / "request.json";
    const auto canonical = json::Parse(Read(contract));
    CHECK(canonical && canonical->IsObject());
    std::vector<std::string> theirs, ours;
    for (const auto& [key, value] : canonical->object) theirs.push_back(key);
    for (const IncidentField& field : package.fields) ours.emplace_back(field.key);
    std::sort(theirs.begin(), theirs.end());
    std::sort(ours.begin(), ours.end());
    CHECK(theirs == ours && theirs.size() == 17);

    // The description is limited to 500 characters, the log to 64 KiB.
    const auto long_description = BuildIncidentPackage(std::string(900, 'x'), log, Report(), Facts());
    CHECK(long_description && CountCharacters(long_description->Value("description")) == kMostDescriptionCharacters);
    const auto euro = BuildIncidentPackage(std::string("\xE2\x82\xAC") + std::string(699, 'x') + std::string(300, 'y'), log, Report(), Facts());
    CHECK(euro && CountCharacters(euro->Value("description")) == kMostDescriptionCharacters);
    CHECK(!BuildIncidentPackage("too short", log, Report(), Facts()));
    CHECK(!BuildIncidentPackage("          ", log, Report(), Facts()));
    CHECK(!BuildIncidentPackage(description, LogOf(folder / "missing.log"), Report(), Facts()));
    CHECK(!BuildIncidentPackage(description, LogOf(MakeLog(folder, 13, "\n  \n", 5)), Report(), Facts()));

    std::string big = kHead;
    for (int line = 0; big.size() < 400 * 1024; ++line) big += "frame " + std::to_string(line) + " presented in 16.6 ms, nothing unusual here\n";
    const GameLog big_log = LogOf(MakeLog(folder, 14, big, 5));
    const auto clipped = BuildIncidentPackage(description, big_log, Report(), Facts());
    CHECK(clipped && clipped->Value("log_text").size() <= kMostLogBytes && clipped->Value("log_clipped") == "yes");
    CHECK(clipped && clipped->Value("log_text").starts_with("frame ") && clipped->Value("log_text").back() == '\n');
    CHECK(clipped && clipped->Value("gpu") == "NVIDIA GeForce RTX 4070" && clipped->Value("renderer") == "Vulkan");
    CHECK(clipped && !Has(clipped->Value("log_text"), "GPU: Vulkan device"));
    if (clipped) CheckAgainstServiceRules(*clipped);

    ReportFacts odd = Report();
    odd.game_version = "1.00 beta";
    odd.region = std::string(200, 'r');
    odd.cusa = "XXXX1";
    odd.eboot_sha256 = "NOT-HEX";
    odd.game_build = "  Build\twith\x01 control  ";
    const auto odd_package = BuildIncidentPackage(description, log, odd, Facts());
    CHECK(odd_package && odd_package->Value("game_version").empty() && odd_package->Value("region").size() == 64 && odd_package->Value("cusa").empty() &&
          odd_package->Value("eboot_sha256").empty() && odd_package->Value("game_build") == "Build with  control");
    if (odd_package) CheckAgainstServiceRules(*odd_package);

    const fs::path saved = SaveIncidentReport(package, scratch / "saved");
    CHECK(!saved.empty() && Read(saved) == preview);
}

struct Wire {
    std::vector<std::pair<std::string, std::string>> calls;
    HttpReply reply;
    HttpLimits limits;
    PostJson Transport() {
        return [this](const std::string& url, const std::string& body, const HttpLimits& given) {
            calls.emplace_back(url, body);
            limits = given;
            return reply;
        };
    }
};

ServiceConfig Configured(Wire& wire, std::vector<std::string>* lines = nullptr) {
    ServiceConfig config;
    config.endpoint = ResolveEndpoint("https://reports.example", "");
    config.post = wire.Transport();
    if (lines) config.log = [lines](const std::string& line) { lines->push_back(line); };
    return config;
}

IncidentPackage SamplePackage(const fs::path& scratch) {
    const GameLog log = LogOf(MakeLog(scratch / "wire", 40, kHead + "frame\n", 1));
    return *BuildIncidentPackage("The game froze on the loading screen", log, Report(), PrivateFacts{});
}

const std::string kReceipt(64, 'b');

// The nth ticket in the service's alphabet (no I, O, 0 or 1).
std::string TicketNumber(int n) {
    constexpr std::string_view kAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    return std::string("SOB-AAAA") + kAlphabet[size_t(n / 10)] + kAlphabet[size_t(n % 10)];
}

TrackedReport Tracked(const std::string& ticket) {
    TrackedReport report;
    report.ticket = ticket;
    report.receipt = kReceipt;
    return report;
}

void TestClient(const fs::path& scratch) {
    const IncidentPackage package = SamplePackage(scratch);
    Wire wire;
    std::vector<std::string> lines;
    ServiceConfig config = Configured(wire, &lines);
    CHECK(wire.calls.empty());

    wire.reply = {HttpFailure::kNone, 201, R"({"ticket":"SOB-ABCDEF","receipt":")" + kReceipt + R"(","submitted_at":1700000000000})"};
    ReportReceipt sent = FileReport(config, package);
    CHECK(sent.outcome == ReportOutcome::kSent && sent.ticket == "SOB-ABCDEF" && sent.credential == kReceipt && sent.submitted_at == 1700000000000);
    CHECK(wire.calls.size() == 1 && wire.calls[0].first == "https://reports.example/v1/reports");
    CHECK(wire.calls[0].second == IncidentRequestJson(package));
    CHECK(wire.limits.connect_ms <= 15000 && wire.limits.total_ms <= 60000 && wire.limits.total_ms > 0 && wire.limits.most_reply_bytes <= 1 << 20);
    for (const std::string& line : lines) CHECK(!Has(line, kReceipt) && !Has(line, "froze") && !Has(line, package.Value("log_text").substr(0, 20)));
    CHECK(!lines.empty());

    const struct {
        HttpFailure failure;
        int status;
        std::string body;
        ReportOutcome outcome;
    } cases[] = {
        {HttpFailure::kUnreachable, 0, "", ReportOutcome::kUnreachable},
        {HttpFailure::kTimedOut, 0, "", ReportOutcome::kTimedOut},
        {HttpFailure::kCertificate, 0, "", ReportOutcome::kCertificate},
        {HttpFailure::kInvalidAddress, 0, "", ReportOutcome::kNotConfigured},
        {HttpFailure::kClientMissing, 0, "", ReportOutcome::kClientMissing},
        {HttpFailure::kNone, 429, R"({"error":"hourly_limit"})", ReportOutcome::kRecentLimit},
        {HttpFailure::kNone, 429, R"({"error":"limit"})", ReportOutcome::kRecentLimit},
        {HttpFailure::kNone, 429, R"({"error":"daily_limit"})", ReportOutcome::kDailyLimit},
        {HttpFailure::kNone, 429, "not json", ReportOutcome::kRecentLimit},
        {HttpFailure::kNone, 400, R"({"error":"bad_request"})", ReportOutcome::kRefused},
        {HttpFailure::kNone, 413, R"({"error":"too_large"})", ReportOutcome::kRefused},
        {HttpFailure::kNone, 403, R"({"error":"origin_not_allowed"})", ReportOutcome::kRefused},
        {HttpFailure::kNone, 500, "oops", ReportOutcome::kServiceError},
        {HttpFailure::kNone, 502, R"({"error":"unavailable"})", ReportOutcome::kServiceError},
        {HttpFailure::kNone, 201, "not json", ReportOutcome::kServiceError},
        {HttpFailure::kNone, 201, "[]", ReportOutcome::kServiceError},
        {HttpFailure::kNone, 201, R"({"ticket":"XXX-ABCDEF","receipt":")" + kReceipt + R"("})", ReportOutcome::kServiceError},
        {HttpFailure::kNone, 201, R"({"ticket":"SOB-ABCDEF","receipt":"short"})", ReportOutcome::kServiceError},
    };
    for (const auto& item : cases) {
        wire.reply = {item.failure, item.status, item.body};
        const ReportReceipt answer = FileReport(config, package);
        CHECK(answer.outcome == item.outcome && answer.ticket.empty() && answer.credential.empty());
        CHECK(OutcomeTitle(item.outcome) && OutcomeAdvice(item.outcome));
    }
    CHECK(!OutcomeTitle(ReportOutcome::kSent) && !OutcomeAdvice(ReportOutcome::kSent));
    CHECK(Has(OutcomeAdvice(ReportOutcome::kUnreachable), "firewall") && Has(OutcomeAdvice(ReportOutcome::kUnreachable), "443"));
    CHECK(std::string(OutcomeTitle(ReportOutcome::kRecentLimit)) == "Too many reports were sent recently");
    CHECK(std::string(OutcomeTitle(ReportOutcome::kDailyLimit)) == "Daily report limit reached");
    CHECK(std::string(OutcomeAdvice(ReportOutcome::kRecentLimit)) == "Please try again later" && std::string(OutcomeAdvice(ReportOutcome::kDailyLimit)) == "Please try again later");
    CHECK(std::string(OutcomeTitle(ReportOutcome::kUnreachable)) == "Cannot reach the report service");
    CHECK(Has(OutcomeAdvice(ReportOutcome::kUnreachable), "press Send report again") && Has(OutcomeAdvice(ReportOutcome::kUnreachable), "your text is kept"));
    CHECK(std::string(OutcomeTitle(ReportOutcome::kServiceError)) == "The report service could not take the report");
    CHECK(Has(OutcomeTitle(ReportOutcome::kClientMissing), "curl") && Has(OutcomeAdvice(ReportOutcome::kClientMissing), "Install curl") &&
          Has(OutcomeAdvice(ReportOutcome::kClientMissing), "your text is kept"));
    for (const ReportOutcome outcome : {ReportOutcome::kUnreachable, ReportOutcome::kServiceError, ReportOutcome::kNotConfigured, ReportOutcome::kRefused}) {
        CHECK(!Has(OutcomeTitle(outcome), "online service") && !Has(OutcomeAdvice(outcome), "not set up") && !Has(OutcomeAdvice(outcome), "not deployed"));
    }

    // Offline and no service: the transport is never asked.
    Wire silent;
    ServiceConfig unset;
    unset.post = silent.Transport();
    CHECK(FileReport(unset, package).outcome == ReportOutcome::kNotConfigured);
    CHECK(FetchReportStatuses(unset, {Tracked("SOB-ABCDEF")}).failure == ReportOutcome::kNotConfigured);
    CHECK(FetchSentReport(unset, Tracked("SOB-ABCDEF")).failure == ReportOutcome::kNotConfigured);
    CHECK(silent.calls.empty());
    CHECK(FetchReportStatuses(config, {}).reached == false);

    // Status lookups carry the receipts and nothing else.
    std::vector<TrackedReport> tracked;
    for (int i = 0; i < 40; ++i) {
        TrackedReport report;
        report.ticket = TicketNumber(i);
        report.receipt = kReceipt;
        report.description = "SECRET DESCRIPTION";
        tracked.push_back(report);
    }
    wire.calls.clear();
    wire.reply = {HttpFailure::kNone, 200,
                  R"({"reports":[{"ticket":"SOB-AAAAAA","status":"fixed","status_updated_at":5,"fixed_version":"0.1.1","player_message":"Fixed."},)"
                  R"({"ticket":"nonsense","status":"closed"}]})"};
    const StatusCheck check = FetchReportStatuses(config, tracked);
    CHECK(check.reached && check.statuses.size() == 1);
    CHECK(check.statuses[0].ticket == "SOB-AAAAAA" && check.statuses[0].status == "fixed" && check.statuses[0].updated_at == 5 &&
          check.statuses[0].fixed_version == "0.1.1" && check.statuses[0].player_message == "Fixed.");
    CHECK(wire.calls.size() == 1 && wire.calls[0].first == "https://reports.example/v1/reports/status");
    const auto asked = json::Parse(wire.calls[0].second);
    CHECK(asked && asked->object.size() == 1 && asked->Find("reports")->array.size() == kMostTrackedReports);
    CHECK(!Has(wire.calls[0].second, "SECRET") && !Has(wire.calls[0].second, "description"));
    for (const json::Value& claim : asked->Find("reports")->array) CHECK(claim.object.size() == 2 && IsTicket(claim.StringOr("ticket")) && IsReceipt(claim.StringOr("receipt")));
    wire.reply = {HttpFailure::kTimedOut, 0, ""};
    const StatusCheck late = FetchReportStatuses(config, tracked);
    CHECK(!late.reached && late.failure == ReportOutcome::kTimedOut && late.statuses.empty());
    wire.reply = {HttpFailure::kNone, 200, R"({"nope":1})"};
    CHECK(!FetchReportStatuses(config, tracked).reached);

    // A previously sent report is read back with the receipt.
    wire.calls.clear();
    wire.reply = {HttpFailure::kNone, 200, R"({"report":)" + IncidentRequestJson(package) + "}"};
    const SentReportRead read = FetchSentReport(config, tracked[0]);
    CHECK(read.reached && read.package && read.package->Value("log_text") == package.Value("log_text"));
    CHECK(wire.calls[0].first == "https://reports.example/v1/reports/read" && wire.calls[0].second.find("SECRET") == std::string::npos);
    wire.reply = {HttpFailure::kNone, 404, R"({"error":"not_found"})"};
    const SentReportRead gone = FetchSentReport(config, tracked[0]);
    CHECK(gone.reached && !gone.package);
    wire.reply = {HttpFailure::kUnreachable, 0, ""};
    CHECK(!FetchSentReport(config, tracked[0]).reached);

    TrackedReport remembered = TrackedReportOf(package, sent);
    CHECK(remembered.ticket == "SOB-ABCDEF" && remembered.receipt == kReceipt && remembered.status == "new" && remembered.gpu == "NVIDIA GeForce RTX 4070");
    CHECK(remembered.log_file == "shadow_of_the_beast_040.log" && remembered.description == package.Value("description"));
}

void TestTransportAddress() {
    ServiceAddress address;
    CHECK(ParseServiceAddress("https://reports.example", address) && address.port == 443 && address.path == "/");
    CHECK(ParseServiceAddress("https://reports.example:8443/v1/reports", address) && address.port == 8443 && address.path == "/v1/reports");
    CHECK(!ParseServiceAddress("http://reports.example", address) && !ParseServiceAddress("http://127.0.0.1:8792", address));
    const HttpReply invalid = PostJsonHttps("http://reports.example/v1/reports", "{}", HttpLimits{});
    CHECK(invalid.failure == HttpFailure::kInvalidAddress && invalid.status == 0);
    CHECK(PostJsonHttps("", "{}", HttpLimits{}).failure == HttpFailure::kInvalidAddress);
#ifndef _WIN32
    // Without curl on PATH the transport says so instead of reporting the service unreachable; nothing is sent.
    const char* path = std::getenv("PATH");
    const std::string saved = path ? path : "";
    setenv("PATH", "/nonexistent-sotb-bug-report-test", 1);
    const HttpReply no_curl = PostJsonHttps("https://reports.example/v1/reports", "{}", HttpLimits{});
    setenv("PATH", saved.c_str(), 1);
    CHECK(no_curl.failure == HttpFailure::kClientMissing && no_curl.status == 0);
    CHECK(OutcomeOfFailure(no_curl.failure) == ReportOutcome::kClientMissing);
#endif
}

void TestHistory(const fs::path& scratch) {
    const fs::path file = scratch / "user" / kReportHistoryFile;
    ReportHistory history(file);
    history.Load();
    CHECK(history.reports().empty() && !history.HasUnseen());
    const auto make = [](const std::string& ticket, int64_t at) {
        TrackedReport report;
        report.ticket = ticket;
        report.receipt = kReceipt;
        report.submitted_at = at;
        report.description = "It froze \"hard\"\nand twice";
        report.gpu = "NVIDIA";
        return report;
    };
    CHECK(history.Add(make("SOB-AAAAAA", 100)) && history.Add(make("SOB-BBBBBB", 200)));
    CHECK(history.reports().size() == 2 && history.reports()[0].ticket == "SOB-BBBBBB");
    CHECK(history.Add(make("SOB-AAAAAA", 300)) && history.reports().size() == 2 && history.reports()[0].submitted_at == 300);

    ReportHistory again(file);
    again.Load();
    CHECK(again.reports().size() == 2 && again.reports()[0].description == "It froze \"hard\"\nand twice" && again.reports()[0].receipt == kReceipt);
    const std::string saved = Read(file);
    for (const char* identity : {"player", "player_id", "install", "install_id", "machine", "user_name", "account", "guid", "uuid", "ip", "address"}) {
        CHECK(!Has(saved, std::string("\"") + identity + "\":"));
    }

    CHECK(!again.Apply({{"SOB-AAAAAA", "bogus", 5, "", ""}, {"SOB-ZZZZZZ", "fixed", 5, "", ""}}));
    CHECK(again.Apply({{"SOB-AAAAAA", "fixed", 500, "0.1.1", "Fixed in the next build."}}));
    CHECK(again.HasUnseen() && again.HasUnseenMessage() && ReportStatusLine(again.reports()[0]) == "Fixed in version 0.1.1");
    CHECK(!again.Apply({{"SOB-AAAAAA", "fixed", 500, "0.1.1", "Fixed in the next build."}}));
    CHECK(again.MarkSeen("SOB-AAAAAA") && !again.HasUnseen() && !again.MarkSeen("SOB-AAAAAA"));
    CHECK(again.Save());
    ReportHistory third(file);
    third.Load();
    CHECK(third.reports()[0].status == "fixed" && third.reports()[0].player_message == "Fixed in the next build." && !third.reports()[0].unseen);
    CHECK(third.Remove("SOB-AAAAAA") && !third.Remove("SOB-AAAAAA") && third.reports().size() == 1);

    for (int i = 0; i < 40; ++i) {
        third.Add(make(TicketNumber(i), 1000 + i));
    }
    CHECK(third.reports().size() == kMostTrackedReports);

    Write(file, R"({"version":1,"reports":[{"ticket":"SOB-AAAAAA","receipt":"x","status":"new"},{"ticket":"bad","receipt":")" + kReceipt +
                    R"(","status":"new"},{"ticket":"SOB-CCCCCC","receipt":")" + kReceipt + R"(","status":"weird"},{"ticket":"SOB-DDDDDD","receipt":")" +
                    kReceipt + R"(","status":"closed"}]})");
    ReportHistory filtered(file);
    filtered.Load();
    CHECK(filtered.reports().size() == 1 && filtered.reports()[0].ticket == "SOB-DDDDDD");
    Write(file, "{ not json");
    ReportHistory damaged(file);
    damaged.Load();
    CHECK(damaged.reports().empty() && fs::exists(fs::path(file) += ".damaged") && !fs::exists(file));
    CHECK(ReportPreview(std::string(100, 'x')).size() == 63 && ReportPreview("a\nb") == "a b");

    Write(file, R"({"version":1,"reports":[{"ticket":"SOB-AAAAAA","receipt":")" + kReceipt + R"(","status":"investigating","unseen":true,"player_message":"Looking.","unknown_future_field":[1,2]}]})");
    ReportHistory legacy(file);
    legacy.Load();
    CHECK(legacy.reports().size() == 1 && !legacy.reports()[0].unavailable && legacy.reports()[0].region.empty() && !legacy.reports()[0].message_unseen);
}

void TestUnavailableReports(const fs::path& scratch) {
    const fs::path file = scratch / "unavailable" / kReportHistoryFile;
    ReportHistory history(file);
    const auto make = [](const std::string& ticket, int64_t at) {
        TrackedReport report;
        report.ticket = ticket;
        report.receipt = kReceipt;
        report.submitted_at = at;
        return report;
    };
    history.Add(make("SOB-AAAAAA", 100));
    history.Add(make("SOB-BBBBBB", 200));

    CHECK(history.Apply({{"SOB-AAAAAA", "new", 100, "", ""}}, {"SOB-AAAAAA", "SOB-BBBBBB"}));
    CHECK(!history.reports()[1].unavailable && history.reports()[0].unavailable);
    CHECK(!history.Apply({{"SOB-AAAAAA", "new", 100, "", ""}}, {"SOB-AAAAAA", "SOB-BBBBBB"}));
    CHECK(history.Save());
    ReportHistory reloaded(file);
    reloaded.Load();
    CHECK(reloaded.reports()[0].unavailable && reloaded.reports()[0].ticket == "SOB-BBBBBB");
    CHECK(reloaded.Apply({{"SOB-BBBBBB", "new", 200, "", ""}}, {"SOB-BBBBBB"}));
    CHECK(!reloaded.reports()[0].unavailable);

    ReportHistory unseen(scratch / "unseen" / kReportHistoryFile);
    unseen.Add(make("SOB-AAAAAA", 100));
    unseen.Add(make("SOB-BBBBBB", 200));
    unseen.Add(make("SOB-CCCCCC", 300));
    CHECK(unseen.NextUnseen().empty());
    CHECK(unseen.Apply({{"SOB-AAAAAA", "investigating", 500, "", ""}, {"SOB-BBBBBB", "fixed", 400, "0.1.1", "Try the new version."},
                        {"SOB-CCCCCC", "closed", 600, "", ""}}));
    CHECK(unseen.NextUnseen() == "SOB-BBBBBB");
    CHECK(unseen.MarkSeen("SOB-BBBBBB") && unseen.NextUnseen() == "SOB-CCCCCC");
    CHECK(unseen.HasUnseen() && unseen.MarkSeen("SOB-AAAAAA") && unseen.HasUnseen() && unseen.MarkSeen("SOB-CCCCCC") && !unseen.HasUnseen());

    ReportHistory gone(scratch / "gone" / kReportHistoryFile);
    gone.Add(make("SOB-AAAAAA", 100));
    CHECK(gone.Apply({{"SOB-AAAAAA", "fixed", 500, "0.1.1", "Try this."}}));
    CHECK(gone.HasUnseen() && gone.HasUnseenMessage() && gone.NextUnseen() == "SOB-AAAAAA");
    CHECK(gone.Apply({}, {"SOB-AAAAAA"}));
    CHECK(!gone.HasUnseen() && !gone.HasUnseenMessage() && gone.NextUnseen().empty());
    const TrackedReport& last = gone.reports()[0];
    CHECK(last.unavailable && last.status == "fixed" && last.fixed_version == "0.1.1" && last.player_message == "Try this.");
    CHECK(gone.Save());
    ReportHistory gone_again(scratch / "gone" / kReportHistoryFile);
    gone_again.Load();
    CHECK(gone_again.reports()[0].unavailable && !gone_again.HasUnseen() && gone_again.reports()[0].player_message == "Try this.");
}

std::string Source(const char* relative) { return Read(fs::path(SOTB_SOURCE_DIR) / relative); }

size_t Count(const std::string& text, std::string_view part) {
    size_t count = 0;
    for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++count;
    return count;
}

void TestShadowLogSessions(const fs::path& scratch) {
    const fs::path folder = scratch / "sessions";
    const std::string crashed_launcher = "[run] 2026-11-19T21:00:00\nStage: launcher handoff\nLauncher: window 1728x918 at 1.50x display scale\n";
    const std::string quit_in_launcher = crashed_launcher + "Launcher: closed\nStage: launcher returned\n";
    const std::string skipped_launcher = "[run]\nStage: launcher handoff\nStage: launcher returned\nBuild fingerprint: CUSA03762 1.00 Korea\n";

    const fs::path running = MakeLog(folder, 60, crashed_launcher, 0);
    for (int i = 0; i < 6; ++i) MakeLog(folder, 59 - i, crashed_launcher, 10 + i);
    MakeLog(folder, 52, quit_in_launcher, 30);
    MakeLog(folder, 40, kHead + "frame 1\n", 100);
    MakeLog(folder, 39, skipped_launcher, 110);

    const std::vector<GameLog> logs = FindGameLogs(folder, running);
    CHECK(logs.size() == kMostGameLogs);
    CHECK(logs[0].name == "shadow_of_the_beast_060.log" && logs[0].role == LogRole::kCurrentLauncher);
    const auto last = std::find_if(logs.begin(), logs.end(), [](const GameLog& log) { return log.role == LogRole::kLastGameplay; });
    CHECK(last != logs.end() && last->name == "shadow_of_the_beast_040.log" && last->activity == LogActivity::kGameplay);
    CHECK(DefaultGameLog(logs) == size_t(last - logs.begin()));
    for (const GameLog& log : logs) CHECK(log.name != "shadow_of_the_beast_052.log");

    const fs::path other = scratch / "skipped";
    const fs::path other_running = MakeLog(other, 10, crashed_launcher, 0);
    MakeLog(other, 9, skipped_launcher, 5);
    MakeLog(other, 8, quit_in_launcher, 6);
    const std::vector<GameLog> started = FindGameLogs(other, other_running);
    CHECK(started.size() == 2 && started[1].name == "shadow_of_the_beast_009.log" && started[1].activity == LogActivity::kGameStarted);
    CHECK(started[1].role == LogRole::kNone && DefaultGameLog(started) == 1);

    CHECK(std::string(LogRoleText(LogRole::kCurrentLauncher)) == "Current launcher session");
    CHECK(std::string(LogRoleText(LogRole::kLastGameplay)) == "Last gameplay");
    CHECK(std::string(LogRoleText(LogRole::kOtherGameplay)) == "Gameplay session");
    CHECK(std::string(LogRoleText(LogRole::kOtherLauncher)) == "Launcher session");
    CHECK(LogRoleText(LogRole::kNone) == nullptr);
}

void TestTechnicalDetailsSurvive() {
    const std::string technical =
        "Exception: ACCESS_VIOLATION at guest 0x00000008012a4b10 (rva 0x1c9820, offset 0x305), access write\n"
        "Stack: 0x00000008012a4b10 0x00000008012a4c88 libc.prx+0x4a2f0 rbp=0x7ffee0123450\n"
        "Build fingerprint: CUSA03762 1.00 Korea, region Korea, title CUSA03762, version 1.00, code CUSA03762-1.00\n"
        "Vulkan: validation warning VUID-vkCmdDraw-None-02699 for VK_LAYER_NV_optimus\n"
        "Executable SHA-256 ebf1d2fa6f44f05ab141e9b06fd8e60179db3be8b0b57b794ce79448ed133023\n"
        "Window fit: bounds 1920x1080, usable 1920x1020, scale 1.50, requested 1920x1080, fitted 1714x964\n"
        "Runtime: guest thread 'SceFiosIO /' created (stack=81920, prio=260, live=3)\n";
    CHECK(ScrubText(technical, PrivateFacts{}) == technical);
}

void TestSecretsAndAddresses() {
    const std::string scrubbed = ScrubText(
        "Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.payload.signature\n"
        "Cookie: session=abc123def456; theme=dark\n"
        "GET https://example.invalid/path?token=abc123&lang=en HTTP/1.1\n"
        "secret: s3cr3t-value\n"
        "api_key=AKIA0123456789ABCDEF\n"
        "peer 192.168.1.20 and 10.0.0.7 and fe80::1ff:fe23:4567:890a and ::1\n",
        PrivateFacts{});
    for (const char* leaked : {"eyJhbGciOi", "abc123def456", "token=abc123", "s3cr3t-value", "AKIA0123456789ABCDEF", "192.168.1.20", "10.0.0.7",
                               "fe80::1ff"}) {
        if (Has(scrubbed, leaked)) std::printf("  leaked: %s\n", leaked);
        CHECK(!Has(scrubbed, leaked));
    }
    CHECK(Has(scrubbed, "<ipv4>") && Has(scrubbed, "<ipv6>"));
    const std::string versions = "Vulkan instance 1.4.357, driver NVIDIA 621.1.0.0, SDL 3.2.8\n";
    CHECK(ScrubText(versions, PrivateFacts{}) == versions);
}

void TestRetryAfterAndCancellation(const fs::path& scratch) {
    CHECK(ParseRetryAfter("60") == 60 && ParseRetryAfter(" 120 \r") == 120 && ParseRetryAfter("0") == 0 && ParseRetryAfter("") == 0);
    CHECK(ParseRetryAfter("Wed, 21 Oct 2026 07:28:00 GMT") == 0 && ParseRetryAfter("-5") == 0 && ParseRetryAfter("1e3") == 0);
    CHECK(ParseRetryAfter("99999999") == 24 * 60 * 60 && ParseRetryAfter("12345678901") == 0);
    CHECK(RetryAfterText(0).empty() && RetryAfterText(-3).empty());
    CHECK(RetryAfterText(30) == "Try again in about 30 seconds." && RetryAfterText(120) == "Try again in about 2 minutes.");
    CHECK(RetryAfterText(7200) == "Try again in about 2 hours.");

    const IncidentPackage package = SamplePackage(scratch);
    Wire wire;
    ServiceConfig config = Configured(wire);
    wire.reply = {HttpFailure::kNone, 429, R"({"error":"hourly_limit"})"};
    wire.reply.retry_after_seconds = 60;
    const ReportReceipt limited = FileReport(config, package);
    CHECK(limited.outcome == ReportOutcome::kRecentLimit && limited.retry_after_seconds == 60 && limited.ticket.empty() && limited.credential.empty());
    wire.reply.retry_after_seconds = 0;
    CHECK(FileReport(config, package).retry_after_seconds == 0);

    config.cancel = std::make_shared<std::atomic<bool>>(false);
    wire.reply = {HttpFailure::kNone, 201, R"({"ticket":"SOB-ABCDEF","receipt":")" + kReceipt + R"(","submitted_at":1})"};
    wire.calls.clear();
    CHECK(FileReport(config, package).outcome == ReportOutcome::kSent && wire.limits.cancel == config.cancel.get());
    config.cancel->store(true);
    wire.calls.clear();
    CHECK(FileReport(config, package).outcome == ReportOutcome::kCancelled);
    CHECK(FetchReportStatuses(config, {Tracked("SOB-ABCDEF")}).failure == ReportOutcome::kCancelled);
    CHECK(FetchSentReport(config, Tracked("SOB-ABCDEF")).failure == ReportOutcome::kCancelled);
    CHECK(wire.calls.empty());
    CHECK(OutcomeOfFailure(HttpFailure::kCancelled) == ReportOutcome::kCancelled && OutcomeTitle(ReportOutcome::kCancelled) &&
          OutcomeAdvice(ReportOutcome::kCancelled));

    std::atomic<bool> stop(true);
    HttpLimits limits;
    limits.cancel = &stop;
    CHECK(PostJsonHttps("https://reports.example/v1/reports", "{}", limits).failure == HttpFailure::kCancelled);
}

void TestNewReplyAndUpdated(const fs::path& scratch) {
    ReportHistory history(scratch / "replies" / kReportHistoryFile);
    TrackedReport report;
    report.ticket = "SOB-AAAAAA";
    report.receipt = kReceipt;
    report.submitted_at = 100;
    history.Add(report);
    CHECK(!history.HasUnseen() && !history.HasUnseenMessage());

    CHECK(history.Apply({{"SOB-AAAAAA", "investigating", 200, "", ""}}));
    CHECK(history.HasUnseen() && !history.HasUnseenMessage() && !history.reports()[0].message_unseen);
    CHECK(history.Apply({{"SOB-AAAAAA", "investigating", 300, "", "Please send a newer log."}}));
    CHECK(history.HasUnseenMessage() && history.reports()[0].player_message == "Please send a newer log.");
    CHECK(!history.Apply({{"SOB-AAAAAA", "investigating", 300, "", "Please send a newer log."}}));
    CHECK(history.HasUnseenMessage());

    CHECK(history.Save());
    ReportHistory reopened(scratch / "replies" / kReportHistoryFile);
    reopened.Load();
    CHECK(reopened.reports().size() == 1 && reopened.reports()[0].message_unseen && reopened.reports()[0].unseen);
    CHECK(reopened.NextUnseen() == "SOB-AAAAAA");
    CHECK(reopened.MarkSeen("SOB-AAAAAA") && !reopened.HasUnseen() && !reopened.HasUnseenMessage());
    CHECK(reopened.reports()[0].player_message == "Please send a newer log.");

    CHECK(reopened.Apply({{"SOB-AAAAAA", "fixed", 400, "0.1.1", "Please send a newer log."}}));
    CHECK(reopened.HasUnseen() && !reopened.HasUnseenMessage());
    CHECK(reopened.Apply({{"SOB-AAAAAA", "fixed", 500, "0.1.1", "Try version 0.1.1."}}));
    CHECK(reopened.HasUnseenMessage() && ReportStatusLine(reopened.reports()[0]) == "Fixed in version 0.1.1");

    const std::string saved = reopened.Dump();
    CHECK(Has(saved, kReceipt) && !Has(reopened.reports()[0].description, kReceipt));
}

void TestReceiptNeverLogged(const fs::path& scratch) {
    const IncidentPackage package = SamplePackage(scratch);
    Wire wire;
    std::vector<std::string> lines;
    ServiceConfig config = Configured(wire, &lines);
    for (const HttpReply& reply : {HttpReply{HttpFailure::kNone, 201, R"({"ticket":"SOB-ABCDEF","receipt":")" + kReceipt + R"("})"},
                                   HttpReply{HttpFailure::kNone, 500, "receipt " + kReceipt}, HttpReply{HttpFailure::kUnreachable, 0, ""}}) {
        wire.reply = reply;
        FileReport(config, package);
        FetchReportStatuses(config, {Tracked("SOB-ABCDEF")});
        FetchSentReport(config, Tracked("SOB-ABCDEF"));
    }
    CHECK(!lines.empty());
    for (const std::string& line : lines) CHECK(!Has(line, kReceipt) && !Has(line, "receipt"));
    for (const char* name : {"launcher/incident_page.cpp", "launcher/my_reports_page.cpp", "launcher/incident_client.cpp", "launcher/report_history.cpp"}) {
        const std::string text = Source(name);
        for (size_t at = text.find("printf("); at != std::string::npos; at = text.find("printf(", at + 1)) {
            const std::string call = text.substr(at, text.find(';', at) - at);
            CHECK(!Has(call, "receipt") && !Has(call, "credential"));
        }
    }
}

// The launcher sends and listens for nothing on its own: connections are made only by the calls below, and each is made
// from a button the player presses.
void TestNothingHappensByItself() {
    const std::string page = Source("launcher/incident_page.cpp");
    const std::string mine = Source("launcher/my_reports_page.cpp");
    const std::string launcher = Source("launcher/launcher.cpp");
    CHECK(Count(page, "FileReport(") == 1 && Count(mine, "FetchReportStatuses(") == 1 && Count(mine, "FetchSentReport(") == 1);
    CHECK(Has(page, "if (send) Send(facts);") && Count(page, "Send(facts)") == 1 && Count(page, "delivery_ = std::make_shared<Delivery>()") == 1);
    CHECK(!Has(page, "BeginPopupModal") && !Has(page, "StartDelivery") && !Has(page, "AskToSend"));
    CHECK(Has(page, "if (delivery_ || !service_.endpoint.configured() || !AcceptDraft()) return;"));
    CHECK(Has(mine, "if (ImGui::Button(Tr(check_ ? \"Checking...\" : \"Refresh\"), ImVec2(width, 0))) Refresh(service, true);"));
    CHECK(Count(mine, "Refresh(service, true)") == 1 && Count(mine, "Refresh(service)") == 1);
    CHECK(Has(mine, "if (!report.unavailable || asked_by_player) reports.push_back(report);"));
    CHECK(Count(mine, "MarkSeen(") == 1 && Count(page, "MarkSeen(") == 0 && Count(launcher, "MarkSeen(") == 0);
    CHECK(Has(mine, "if (check_ || !service.endpoint.configured()) return;") && Has(mine, "if (reports.empty()) return;"));
    CHECK(Has(mine, "const bool toggle_sent = ImGui::Button(") && Count(mine, "ReadSentReport(report, service)") == 1);
    CHECK(Count(launcher, "CheckReports()") == 1 && Has(Source("launcher/incident_page.h"), "void CheckReports() { my_reports_.Refresh(service_); }"));

    for (const char* name : {"launcher/launcher.cpp", "launcher/host.cpp", "launcher/options.cpp", "launcher/settings.cpp", "launcher/paths.cpp",
                             "launcher/game_install.cpp", "launcher/game_files.cpp", "launcher/trophy_page.cpp", "src/loader.c", "src/runtime_thread.c"}) {
        const std::string text = Source(name);
        CHECK(!text.empty());
        CHECK(!Has(text, "incident_client.h") && !Has(text, "http_post.h") && !Has(text, "FileReport") && !Has(text, "PostJsonHttps"));
        CHECK(!Has(text, "winhttp") && !Has(text, "WinHttp") && !Has(text, "WSAStartup") && !Has(text, "trg/download.h"));
    }
    CHECK(Has(launcher, "incident_page.h") && !Has(launcher, "FetchReportStatuses"));

    for (const char* name : {"launcher/http_post.cpp", "launcher/incident_client.cpp", "launcher/incident_page.cpp", "launcher/my_reports_page.cpp"}) {
        const std::string text = Source(name);
        for (const char* forbidden : {"listen(", "accept(", "bind(", "socket(", "WSAStartup", "UPnP", "upnp", "STUN", "stun", "netsh", "ShellExecute",
                                      "runas", "AddAuthorizedApplication", "INetFwPolicy", "SECURITY_FLAG_IGNORE", "SECURE_FAILURE_FLAG",
                                      "WINHTTP_OPTION_SECURITY_FLAGS", "std::this_thread::sleep"}) {
            if (Has(text, forbidden)) std::printf("  %s mentions %s\n", name, forbidden);
            CHECK(!Has(text, forbidden));
        }
    }
    const std::string transport = Source("launcher/http_post.cpp");
    CHECK(Has(transport, "WINHTTP_OPTION_REDIRECT_POLICY_NEVER") && Has(transport, "WINHTTP_FLAG_SECURE") && Has(transport, "WinHttpSetTimeouts"));
    CHECK(Has(transport, "\"=https\"") && Has(transport, "\"--max-redirs\"") && Has(transport, "posix_spawnp"));
    CHECK(!Has(transport, "popen(") && !Has(transport, "system("));

    const std::string profile = Source("launcher/game_profile.h");
    CHECK(!Has(profile, "localhost") && !Has(profile, "127.0.0.1") && Has(profile, "kBugReportServiceUrl"));
    CHECK(Source("launcher/incident_page.cpp").find("localhost") == std::string::npos);
    const std::string wrangler = Source("server/shadow-bug-reports/wrangler.toml");
    CHECK(Has(wrangler, "name = \"shadow-bug-reports\"") && Has(wrangler, "new_sqlite_classes = [\"BugReportDesk\"]") &&
          Has(wrangler, "[[ratelimits]]") && !Has(wrangler, "13-sentinels") && !Has(wrangler, "dcp-lobby") && !Has(wrangler, "SECRET"));
    const std::string cmake = Source("launcher/CMakeLists.txt");
    CHECK(Has(cmake, "http_post.cpp") && Has(cmake, "json_lite.cpp"));
    const std::string manifest = Source("src/shadow_of_the_beast.manifest");
    CHECK(!Has(manifest, "requireAdministrator") && !Has(manifest, "highestAvailable"));
}

// The production page: the four steps and status cards, no pre-deployment wording, no other project's names or transport.
void TestProductionPage() {
    const std::string page = Source("launcher/incident_page.cpp");
    const std::string mine = Source("launcher/my_reports_page.cpp");
    const std::string client = Source("launcher/incident_client.cpp");
    const std::string profile = Source("launcher/game_profile.h");

    size_t at = 0;
    for (const char* part : {"No personal user data is intentionally included.", "ui.Heading(\"1. Report the problem\")", "ui.Heading(\"2. Choose one game log\")",
                             "ui.Heading(\"3. Preview what will be sent\")", "ui.Heading(\"4. Send\")"}) {
        const size_t found = page.find(part, at);
        CHECK(found != std::string::npos);
        if (found != std::string::npos) at = found;
    }
    for (const char* part : {"What were you doing, what went wrong, and does it happen every time?", "Please describe the problem, minimum 10 characters",
                             "kDescriptionHeight = 112.0f", "kPreviewHeight = 300.0f", "Send this log", "\"Selected log\"", "\"Last written\"", "\"Size\"",
                             "View selected log", "Shows exactly what will be sent, after the privacy filter.", "PreviewButtonText(previewed_ != nullptr)",
                             "ReadOnlyBox(\"##preview\"", "Sends the selected log after the privacy filter is applied.", "sending ? \"Sending...\" : \"Send report\"",
                             "Sending your report...", "This takes a few seconds.", "Report sent successfully", "Copy report ID", "Report ID copied.",
                             "A report has a new reply", "A report has an update", "ui.StatusCard(trg::Status::kAttention, OutcomeTitle(result_->outcome)"}) {
        if (!Has(page, part)) std::printf("  incident_page.cpp lacks %s\n", part);
        CHECK(Has(page, part));
    }
    CHECK(Has(page, "reply ? \"A report has a new reply\" : \"A report has an update\""));
    CHECK(Has(page, "if (!service_.endpoint.configured()) WarningText(ui, service_.endpoint.problem.c_str());"));
    CHECK(Has(page, "ImGui::BeginDisabled(sending);") && Has(page, "description_[0] = '\\0';"));
    CHECK(Count(page, "description_[0] = '\\0';") == 2);

    for (const char* part : {"New reply", "Message from developer", "No longer available", "What was sent", "Remove from this PC", "View details", "Hide details"}) {
        if (!Has(mine, part)) std::printf("  my_reports_page.cpp lacks %s\n", part);
        CHECK(Has(mine, part));
    }
    CHECK(!Has(mine, "Their status is checked when"));

    for (const std::string* text : {&page, &mine, &client, &profile}) {
        for (const char* stale : {"service is not deployed", "not available in this version",
                                  "attach it to a GitHub issue yourself", "attach a saved copy yourself", "Nothing is sent unless you press", "Use this log",
                                  "after you confirm", "placeholder", "Send this report to the developer?"}) {
            if (Has(*text, stale)) std::printf("  stale text: %s\n", stale);
            CHECK(!Has(*text, stale));
        }
        for (const char* other : {"lobby", "Lobby", "WebSocket", "websocket", "Online Play", "online service", "friend", "DCP-", "dcp::", "dragon", "Dragon",
                                  "DEBUG_SAULO", "SOTB_BUG_REPORT_ADMIN_TOKEN", "Odin", "odin", "Vanillaware", "13 Sentinels"}) {
            if (Has(*text, other)) std::printf("  foreign text: %s\n", other);
            CHECK(!Has(*text, other));
        }
    }
    CHECK(Has(profile, "kBugReportServiceUrl[] = \"https://shadow-bug-reports.saulobenigno.workers.dev\""));
    CHECK(Has(Source("launcher/incident_client.h"), "SOTB_BUG_REPORT_URL"));
    CHECK(Count(page, "kBugReportServiceUrl") == 1 && Has(page, "ResolveEndpoint(kBugReportServiceUrl, EnvironmentValue(kBugReportUrlVariable))"));

    const std::string docs = Source("docs/LAUNCHER.md");
    for (const char* part : {"## Bug Report", "SOTB_BUG_REPORT_URL", "server/shadow-bug-reports", "My Reports", "New reply", "No longer available",
                             "SOTB_BUG_REPORT_ADMIN_TOKEN"}) {
        if (!Has(docs, part)) std::printf("  docs/LAUNCHER.md lacks %s\n", part);
        CHECK(Has(docs, part));
    }
    CHECK(!std::regex_search(docs, std::regex("[0-9a-f]{40,}")));
}

} // namespace

int main() {
    const fs::path scratch = fs::temp_directory_path() / "sotb-bug-report-tests";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);
    TestJson();
    TestEndpoint();
    TestDefaultEndpoint();
    TestLogChoice(scratch);
    TestPrivacyFilter();
    TestDiagnostics();
    TestReport(scratch);
    TestClient(scratch);
    TestTransportAddress();
    TestHistory(scratch);
    TestUnavailableReports(scratch);
    TestShadowLogSessions(scratch);
    TestTechnicalDetailsSurvive();
    TestSecretsAndAddresses();
    TestRetryAfterAndCancellation(scratch);
    TestNewReplyAndUpdated(scratch);
    TestReceiptNeverLogged(scratch);
    TestNothingHappensByItself();
    TestProductionPage();
    fs::remove_all(scratch, ec);
    std::printf("bug report: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
