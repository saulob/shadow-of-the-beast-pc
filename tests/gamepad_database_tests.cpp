// User gamepad database and unmapped joystick diagnostics: path, loader outcomes, once-per-identity log, load timing
// in the runtime source, and real SDL virtual joysticks (no physical controller, no window, no game).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#define SOTB_SDL_COMPAT_POLICY_ONLY
#include <SDL3/SDL.h>

extern "C" {
#include "sotb_pad_mapping.h"
#include "sotb_sdl_compat.h"
}

namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        ++g_checks;                                                                        \
        if (!(condition)) {                                                                \
            ++g_failures;                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);              \
        }                                                                                  \
    } while (0)

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}

bool Contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

struct FakeLoader {
    int64_t size = -1;
    int added = 0;
    const char* error = "";
    int calls = 0;
    int order = 0;
    int size_order = 0;
    int add_order = 0;
};

int64_t FakeSize(const char*, void* user) {
    auto* f = static_cast<FakeLoader*>(user);
    f->size_order = ++f->order;
    return f->size;
}
int FakeAdd(const char*, void* user) {
    auto* f = static_cast<FakeLoader*>(user);
    ++f->calls;
    f->add_order = ++f->order;
    return f->added;
}
const char* FakeError(void* user) { return static_cast<FakeLoader*>(user)->error; }

SotbMappingResult Load(FakeLoader& fake, std::string& line) {
    const SotbMappingLoader loader{FakeSize, FakeAdd, FakeError, &fake};
    char buffer[256];
    const auto result = sotb_load_user_mappings("user/gamecontrollerdb.txt", &loader, buffer, sizeof(buffer));
    line = buffer;
    return result;
}

void TestPathAndLoader() {
    char path[128];
    CHECK(sotb_gamepad_db_path("user/sotb.ini", path, sizeof(path)) && std::string(path) == "user/gamecontrollerdb.txt");
    CHECK(sotb_gamepad_db_path("C:\\game\\user\\sotb.ini", path, sizeof(path)) && std::string(path) == "C:\\game\\user\\gamecontrollerdb.txt");
    CHECK(sotb_gamepad_db_path("sotb.ini", path, sizeof(path)) && std::string(path) == "gamecontrollerdb.txt");
    CHECK(!sotb_gamepad_db_path(nullptr, path, sizeof(path)));
    CHECK(!sotb_gamepad_db_path("", path, sizeof(path)));
    CHECK(!sotb_gamepad_db_path("user/sotb.ini", path, 8));
    CHECK(!sotb_gamepad_db_path("user/sotb.ini", path, 0));

    FakeLoader missing;
    std::string line;
    CHECK(Load(missing, line) == SOTB_MAPPING_ABSENT && line.empty() && missing.calls == 0);

    FakeLoader valid;
    valid.size = 1000;
    valid.added = 3;
    CHECK(Load(valid, line) == SOTB_MAPPING_LOADED && line == "Runtime: 3 gamepad mappings read from user/gamecontrollerdb.txt");
    CHECK(valid.size_order < valid.add_order && valid.calls == 1);

    FakeLoader empty;
    empty.size = 0;
    empty.added = 0;
    CHECK(Load(empty, line) == SOTB_MAPPING_LOADED && Contains(line, "0 gamepad mappings"));

    FakeLoader failing;
    failing.size = 50;
    failing.added = -1;
    failing.error = "Parameter 'file' is invalid\r\nsecond line";
    CHECK(Load(failing, line) == SOTB_MAPPING_FAILED && Contains(line, "not read") && !Contains(line, "\n") && !Contains(line, "\r"));
    failing.error = nullptr;
    CHECK(Load(failing, line) == SOTB_MAPPING_FAILED && Contains(line, "unreadable"));

    FakeLoader huge;
    huge.size = SOTB_MAPPING_FILE_MAX + 1;
    CHECK(Load(huge, line) == SOTB_MAPPING_FAILED && huge.calls == 0 && Contains(line, "larger than"));
    huge.size = SOTB_MAPPING_FILE_MAX;
    huge.added = 1;
    CHECK(Load(huge, line) == SOTB_MAPPING_LOADED && huge.calls == 1);

    FakeLoader many;
    many.size = 10;
    many.added = 2000000000;
    CHECK(Load(many, line) == SOTB_MAPPING_LOADED && Contains(line, "99999 gamepad mappings"));
    many.added = 12;
    CHECK(Load(many, line) == SOTB_MAPPING_LOADED && line.size() < 128);

    char tiny[16];
    const SotbMappingLoader loader{FakeSize, FakeAdd, FakeError, &many};
    CHECK(sotb_load_user_mappings("p", &loader, tiny, sizeof(tiny)) == SOTB_MAPPING_LOADED && std::strlen(tiny) < sizeof(tiny));
    CHECK(sotb_load_user_mappings(nullptr, &loader, tiny, sizeof(tiny)) == SOTB_MAPPING_ABSENT);
    CHECK(sotb_load_user_mappings("p", nullptr, tiny, sizeof(tiny)) == SOTB_MAPPING_ABSENT);
}

int Unmapped(SotbUnmappedSeen& seen, const char* name, const char* guid, unsigned vid, unsigned pid) {
    char key[SOTB_UNMAPPED_KEY];
    sotb_unmapped_key(name, guid, vid, pid, key, sizeof(key));
    return sotb_unmapped_first_sight(&seen, key);
}

void TestUnmappedDiagnostics() {
    SotbUnmappedSeen seen{};
    const char* guid_a = "030000005e0400008e02000000007200";
    const char* guid_b = "03000000ffff0000aaaa000000000000";
    CHECK(Unmapped(seen, "Pad A", guid_a, 0x045e, 0x028e));
    CHECK(!Unmapped(seen, "Pad A", guid_a, 0x045e, 0x028e));
    CHECK(!Unmapped(seen, "Pad A renamed on reconnect", guid_a, 0x045e, 0x028e));
    CHECK(Unmapped(seen, "Pad B", guid_b, 0xffff, 0xaaaa));
    int repeats = 0;
    for (int i = 0; i < 20000; ++i) repeats += Unmapped(seen, i % 2 ? "Pad A" : "Pad B", i % 2 ? guid_a : guid_b, 0, 0);
    CHECK(repeats == 0);

    SotbUnmappedSeen churn{};
    int firsts = 0;
    for (int i = 0; i < 1000; ++i) {
        char guid[33];
        std::snprintf(guid, sizeof(guid), "%032x", i);
        firsts += Unmapped(churn, "x", guid, 1, 2);
    }
    CHECK(firsts == 1000 && churn.count == SOTB_UNMAPPED_SEEN);

    SotbUnmappedSeen noguid{};
    CHECK(Unmapped(noguid, "", "", 0, 0));
    CHECK(!Unmapped(noguid, "", "", 0, 0));
    CHECK(Unmapped(noguid, nullptr, nullptr, 0x1234, 0x5678));
    CHECK(Unmapped(noguid, "Other", nullptr, 0x1234, 0x5678));
    char k1[SOTB_UNMAPPED_KEY], k2[SOTB_UNMAPPED_KEY];
    sotb_unmapped_key("n", guid_a, 1, 2, k1, sizeof(k1));
    sotb_unmapped_key("n", guid_a, 1, 2, k2, sizeof(k2));
    CHECK(std::strcmp(k1, k2) == 0);

    char line[384];
    sotb_unmapped_line("Pad A", guid_a, 0x045e, 0x028e, line, sizeof(line));
    CHECK(std::string(line) == "Runtime: joystick without a gamepad mapping: name=Pad A, guid=030000005e0400008e02000000007200, vid:pid=045e:028e; add a line for this GUID to gamecontrollerdb.txt in the user folder");
    sotb_unmapped_line(nullptr, guid_a, 0, 0, line, sizeof(line));
    CHECK(Contains(line, "name=unknown") && Contains(line, "vid:pid=none"));
    sotb_unmapped_line("", nullptr, 0xabcd, 0x1, line, sizeof(line));
    CHECK(Contains(line, "name=unknown") && Contains(line, "guid=unknown") && Contains(line, "vid:pid=abcd:0001"));
    sotb_unmapped_line("Evil\nName\r\x01", guid_a, 1, 2, line, sizeof(line));
    CHECK(!Contains(line, "\n") && !Contains(line, "\r") && !Contains(line, "\x01"));
    const std::string longname(500, 'N');
    sotb_unmapped_line(longname.c_str(), guid_a, 1, 2, line, sizeof(line));
    CHECK(std::strlen(line) < sizeof(line) && Contains(line, "guid=") && Contains(line, "vid:pid=0001:0002"));
    CHECK(!Contains(line, "0x") && !Contains(line, ":\\") && !Contains(line, "/Users"));
}

void TestRuntimeWiring() {
    const std::string runtime = ReadAll(fs::path(SOTB_SOURCE_DIR) / "src/runtime_pad.c");
    const size_t refresh = runtime.find("static void refresh_gamepads");
    CHECK(refresh != std::string::npos);
    const std::string body = runtime.substr(refresh, runtime.find("Button sources", refresh) - refresh);
    const size_t init = body.find("SDL_InitSubSystem(SDL_INIT_GAMEPAD)");
    const size_t load = body.find("load_user_gamepad_mappings()");
    const size_t unmapped = body.find("log_unmapped_joysticks()");
    const size_t enumerate = body.find("SDL_GetGamepads(");
    CHECK(init != std::string::npos && init < load && load < unmapped && unmapped < enumerate);
    CHECK(Contains(body, "sdl_ready>0") && Contains(body, "if (!sdl_ready)"));
    CHECK(Contains(runtime, "SDL_AddGamepadMappingsFromFile") && !Contains(runtime, "SDL_AddGamepadMapping("));
    const size_t db_start = runtime.find("static int64_t mapping_file_size");
    const std::string db_code = runtime.substr(db_start, runtime.find("static void refresh_gamepads", db_start) - db_start);
    CHECK(!Contains(db_code, "fopen") && !Contains(db_code, "SDL_IOFromFile") && !Contains(db_code, "getenv"));
    CHECK(Contains(runtime, "sotb_gamepad_db_path(keys_file"));
    CHECK(!Contains(runtime, "getenv(\"SOTB_GAMEPAD") && !Contains(runtime, "http"));
    const size_t log_start = runtime.find("static void log_unmapped_joysticks");
    const std::string logger = runtime.substr(log_start, runtime.find("static void refresh_gamepads", log_start) - log_start);
    CHECK(Contains(logger, "SDL_IsGamepad(") && !Contains(logger, "SDL_OpenJoystick") && !Contains(logger, "SDL_OpenGamepad"));
    CHECK(Contains(logger, "sotb_unmapped_first_sight") && !Contains(logger, "ids[j]=="));
    CHECK(Contains(runtime, "sotb_pad_classify(") && Contains(runtime, "sotb_pad_load(keys_file,&pad_map)"));
}

SDL_JoystickID Attach(const char* name, std::uint16_t vendor, std::uint16_t product) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_UNKNOWN;
    desc.naxes = 6;
    desc.nbuttons = 16;
    desc.vendor_id = vendor;
    desc.product_id = product;
    desc.name = name;
    return SDL_AttachVirtualJoystick(&desc);
}

std::string GuidOf(SDL_JoystickID id) {
    char guid[33];
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof(guid));
    return guid;
}

struct RealSdl {
    static int64_t Size(const char* p, void*) {
        SDL_PathInfo info;
        return SDL_GetPathInfo(p, &info) && info.type == SDL_PATHTYPE_FILE ? static_cast<int64_t>(info.size) : -1;
    }
    static int Add(const char* p, void*) { return SDL_AddGamepadMappingsFromFile(p); }
    static const char* Err(void*) { return SDL_GetError(); }
};

void TestRealSdlMappings(const fs::path& base) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    if (!SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
        std::printf("SDL could not start (%s); real SDL mapping checks skipped\n", SDL_GetError());
        return;
    }
    const SDL_JoystickID first = Attach("Test Pad One", 0x1209, 0x0001);
    CHECK(first != 0);
    if (!first) {
        SDL_Quit();
        return;
    }
    CHECK(!SDL_IsGamepad(first));
    const std::string guid = GuidOf(first);
    CHECK(guid.size() == 32 && guid == GuidOf(first));

    const fs::path user = base / "user";
    fs::create_directories(user);
    const fs::path db = user / "gamecontrollerdb.txt";
    char path[512];
    CHECK(sotb_gamepad_db_path((user / "sotb.ini").string().c_str(), path, sizeof(path)));
    CHECK(fs::path(path) == db);
    CHECK(!fs::exists(db));

    const SotbMappingLoader real{RealSdl::Size, RealSdl::Add, RealSdl::Err, nullptr};
    char line[256];
    CHECK(sotb_load_user_mappings(path, &real, line, sizeof(line)) == SOTB_MAPPING_ABSENT);
    CHECK(!fs::exists(db) && !SDL_IsGamepad(first));

    {
        std::ofstream out(db, std::ios::binary);
        out << "# comment\nnot a mapping at all\n"
            << guid << ",Test Pad One,a:b0,b:b1,x:b2,y:b3,back:b4,start:b5,leftshoulder:b6,rightshoulder:b7,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,platform:"
            << SDL_GetPlatform() << ",\n";
    }
    CHECK(sotb_load_user_mappings(path, &real, line, sizeof(line)) == SOTB_MAPPING_LOADED &&
          Contains(line, "Runtime: 1 gamepad mappings read"));
    CHECK(SDL_IsGamepad(first));

    const SDL_JoystickID second = Attach("Test Pad Two", 0x1209, 0x0002);
    CHECK(second != 0 && !SDL_IsGamepad(second) && GuidOf(second) != guid);
    const SDL_JoystickID twin = Attach("Test Pad One", 0x1209, 0x0001);
    CHECK(twin != 0 && SDL_IsGamepad(twin));

    {
        std::ofstream out(db, std::ios::binary | std::ios::trunc);
        out << std::string(64, '\0') << "\nbroken,line\n";
    }
    CHECK(sotb_load_user_mappings(path, &real, line, sizeof(line)) != SOTB_MAPPING_ABSENT);
    CHECK(SDL_IsGamepad(first));
    SDL_Gamepad* pad = SDL_OpenGamepad(first);
    CHECK(pad != nullptr);
    if (pad) {
        CHECK(SDL_GetGamepadVendor(pad) == 0x1209 && SDL_GetGamepadProduct(pad) == 0x0001);
        SDL_CloseGamepad(pad);
    }

    if (twin) SDL_DetachVirtualJoystick(twin);
    if (second) SDL_DetachVirtualJoystick(second);
    SDL_DetachVirtualJoystick(first);
    const SDL_JoystickID again = Attach("Test Pad One", 0x1209, 0x0001);
    CHECK(again != 0 && SDL_IsGamepad(again) && GuidOf(again) == guid);
    if (again) SDL_DetachVirtualJoystick(again);
    SDL_Quit();
}

} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / ("sotb-gamepad-database-tests-" + std::to_string(std::rand()));
    fs::create_directories(base);
    TestPathAndLoader();
    TestUnmappedDiagnostics();
    TestRuntimeWiring();
    TestRealSdlMappings(base);
    std::error_code ignored;
    fs::remove_all(base, ignored);
    std::printf("gamepad database tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
