#include "sotb_title.h"
#include "sotbgpu.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#define CHECK(cond)                                                                         \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            return 1;                                                                       \
        }                                                                                   \
    } while (0)

static bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

static std::string read_text(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main() {
    const std::string base = SOTB_WINDOW_TITLE;

    const std::string t_none = SotbTitle::Compose({
        .base = base,
        .show_resolution = false,
        .width = 2560,
        .height = 1440,
        .show_fps = false,
        .fps = 59.87,
    });
    CHECK(t_none == "Shadow of the Beast");

    const std::string t_fps = SotbTitle::Compose({
        .base = base,
        .show_resolution = false,
        .width = 2560,
        .height = 1440,
        .show_fps = true,
        .fps = 59.87,
    });
    CHECK(t_fps == "Shadow of the Beast | FPS: 59.87");

    const std::string t_res = SotbTitle::Compose({
        .base = base,
        .show_resolution = true,
        .width = 2560,
        .height = 1440,
        .show_fps = false,
        .fps = 59.87,
    });
    CHECK(t_res == "Shadow of the Beast | 2560x1440");

    const std::string t_both = SotbTitle::Compose({
        .base = base,
        .show_resolution = true,
        .width = 2560,
        .height = 1440,
        .show_fps = true,
        .fps = 59.87,
    });
    CHECK(t_both == "Shadow of the Beast | 2560x1440 | FPS: 59.87");

    for (const std::string* t : {&t_none, &t_fps, &t_res, &t_both}) {
        CHECK(!contains(*t, "CUSA"));
        CHECK(!contains(*t, "CUSA03762"));
        CHECK(!contains(*t, "1.00"));
    }

#ifdef SOTB_SOURCE_DIR
    const std::string sotbgpu = read_text(std::string(SOTB_SOURCE_DIR) + "/gpu/shim/sotbgpu.cpp");
    CHECK(!sotbgpu.empty());
    CHECK(!contains(sotbgpu, "SotbTitle::Base"));
    const std::string header = read_text(std::string(SOTB_SOURCE_DIR) + "/gpu/shim/sotb_title.h");
    CHECK(!header.empty());
    CHECK(!contains(header, "CUSA03762"));
#endif

    std::puts("title_bar_tests: PASS");
    return 0;
}
