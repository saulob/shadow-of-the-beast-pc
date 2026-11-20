# The launcher: TRG Launcher (third_party/trg-launcher, fetched by tools\build.ps1) in a window of its own.
set(TRG_DIR ${PROJECT_SOURCE_DIR}/third_party/trg-launcher)
if (NOT EXISTS ${TRG_DIR}/include/trg/launcher.h)
    message(FATAL_ERROR "TRG Launcher is missing: run tools\\build.ps1, which fetches it (gpu\\deps.txt).")
endif()
set(IMGUI_DIR ${PROJECT_SOURCE_DIR}/gpu/third_party/imgui)

add_library(trg_core STATIC ${TRG_DIR}/src/settings.cpp ${TRG_DIR}/src/platform.cpp ${TRG_DIR}/src/download.cpp)
target_include_directories(trg_core PUBLIC ${TRG_DIR}/include)
target_compile_definitions(trg_core PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(trg_core PRIVATE shell32 ole32 comdlg32 winhttp)

add_library(trg_launcher STATIC ${TRG_DIR}/src/launcher.cpp ${TRG_DIR}/src/headers.cpp)
target_include_directories(trg_launcher PUBLIC ${IMGUI_DIR})
target_compile_definitions(trg_launcher PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(trg_launcher PUBLIC trg_core sotbgpu)

set_target_properties(trg_core trg_launcher PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

add_library(sotb_imgui_sdl STATIC ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(sotb_imgui_sdl PUBLIC ${IMGUI_DIR})
target_link_libraries(sotb_imgui_sdl PUBLIC SDL3::SDL3)

find_package(ZLIB REQUIRED)
add_library(sotb_launcher_core STATIC
    boot_image.cpp
    build_diagnostics.cpp
    game_files.cpp
    game_install.cpp
    game_profile.cpp
    header_art.cpp
    http_post.cpp
    incident_client.cpp
    incident_report.cpp
    json_lite.cpp
    log_scrubber.cpp
    options.cpp
    page_icons.cpp
    paths.cpp
    pkg_extract.cpp
    report_history.cpp
    settings.cpp
    text_languages.cpp
    trophy_content.cpp
    ${PROJECT_SOURCE_DIR}/src/trophies/trophy_data.cpp
    ${PROJECT_SOURCE_DIR}/src/trophies/trophy_key.cpp
    ${PROJECT_SOURCE_DIR}/src/trophies/trophy_pack.cpp
    ${PROJECT_SOURCE_DIR}/src/trophies/trophy_runtime.cpp
    ${PROJECT_SOURCE_DIR}/src/trophies/trophy_service.cpp
    ${PROJECT_SOURCE_DIR}/src/trophies/trophy_store.cpp
    ${PROJECT_SOURCE_DIR}/src/trophies/trophy_view.cpp
    ${PROJECT_SOURCE_DIR}/src/trophies/trp_archive.cpp
    crypto_win.cpp)
target_include_directories(sotb_launcher_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(sotb_launcher_core PUBLIC trg_core SDL3::SDL3 sotb_startup_phase sotb_guest_display
    PRIVATE ZLIB::ZLIB bcrypt winhttp)
target_compile_definitions(sotb_launcher_core PRIVATE SOTB_PORT_VERSION="${PROJECT_VERSION}")

# The sidebar's page icons (assets/launcher/icons/*.svg) as a table of byte arrays, page_icon_data.cpp.
file(GLOB SOTB_PAGE_ICONS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/assets/launcher/icons/*.svg)
list(SORT SOTB_PAGE_ICONS)
set(SOTB_PAGE_ICON_LIST ${CMAKE_BINARY_DIR}/generated/page_icons.txt)
file(CONFIGURE OUTPUT ${SOTB_PAGE_ICON_LIST} CONTENT "${SOTB_PAGE_ICONS}")
set(SOTB_PAGE_ICON_SOURCE ${CMAKE_BINARY_DIR}/generated/page_icon_data.cpp)
add_custom_command(OUTPUT ${SOTB_PAGE_ICON_SOURCE}
    COMMAND ${CMAKE_COMMAND} -DINPUT_LIST=${SOTB_PAGE_ICON_LIST} -DOUTPUT=${SOTB_PAGE_ICON_SOURCE} -DSYMBOL=kPageIconFiles
            -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_files.cmake
    DEPENDS ${SOTB_PAGE_ICON_LIST} ${SOTB_PAGE_ICONS} ${CMAKE_CURRENT_SOURCE_DIR}/embed_files.cmake
    COMMENT "Embedding the page icons")
target_sources(sotb_launcher_core PRIVATE ${SOTB_PAGE_ICON_SOURCE})

add_library(sotb_launcher STATIC host.cpp incident_page.cpp launcher.cpp my_reports_page.cpp trophy_page.cpp)
target_compile_definitions(sotb_launcher PRIVATE SOTB_PORT_VERSION="${PROJECT_VERSION}")
target_include_directories(sotb_launcher PRIVATE ${PROJECT_SOURCE_DIR}/gpu/shim)
target_link_libraries(sotb_launcher PUBLIC sotb_launcher_core trg_launcher sotb_imgui_sdl ${CMAKE_DL_LIBS})

set_target_properties(sotb_launcher_core sotb_launcher PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
target_compile_options(sotb_launcher_core PRIVATE -Wall -Wextra)
target_compile_options(sotb_launcher PRIVATE -Wall -Wextra)

find_package(PNG REQUIRED)
set(SOTB_LOG_SOURCES ${PROJECT_SOURCE_DIR}/src/runtime_log.c ${PROJECT_SOURCE_DIR}/src/runtime_session_log.c)

add_executable(sotb-trophy-tests EXCLUDE_FROM_ALL ../tests/trophy_tests.cpp ${SOTB_LOG_SOURCES} ../gpu/shim/sotb_png.cpp)
target_link_libraries(sotb-trophy-tests PRIVATE sotb_launcher_core PNG::PNG sotb_keys)
target_include_directories(sotb-trophy-tests PRIVATE ${PROJECT_SOURCE_DIR}/gpu/shim)
target_compile_definitions(sotb-trophy-tests PRIVATE SOTB_TROPHY_SOURCE_DIR="${PROJECT_SOURCE_DIR}/src/trophies")
set_target_properties(sotb-trophy-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(sotb-trophy-tests PRIVATE -Wall -Wextra)

add_executable(sotb-launcher-tests EXCLUDE_FROM_ALL ../tests/launcher_tests.cpp ../gpu/shim/sotb_settings.cpp)
target_link_libraries(sotb-launcher-tests PRIVATE sotb_launcher_core sotb_keys SDL3::SDL3)
target_include_directories(sotb-launcher-tests PRIVATE ${PROJECT_SOURCE_DIR}/gpu/shim)
target_compile_definitions(sotb-launcher-tests PRIVATE SOTB_BOOT_IMAGE="${PROJECT_SOURCE_DIR}/build/image/CUSA03762-1.00/boot.bin"
    SOTB_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(sotb-launcher-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(sotb-launcher-tests PRIVATE -Wall -Wextra)

add_executable(sotb-bug-report-tests EXCLUDE_FROM_ALL ../tests/bug_report_tests.cpp)
target_link_libraries(sotb-bug-report-tests PRIVATE sotb_launcher_core)
target_compile_definitions(sotb-bug-report-tests PRIVATE SOTB_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(sotb-bug-report-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(sotb-bug-report-tests PRIVATE -Wall -Wextra)

add_executable(sotb-launcher-ui-tests EXCLUDE_FROM_ALL ../tests/launcher_ui_tests.cpp ${TRG_DIR}/src/launcher.cpp
    ${TRG_DIR}/src/headers.cpp ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp)
target_link_libraries(sotb-launcher-ui-tests PRIVATE sotb_launcher_core trg_core sotb_keys SDL3::SDL3)
target_include_directories(sotb-launcher-ui-tests PRIVATE ${IMGUI_DIR} ${PROJECT_SOURCE_DIR}/gpu/shim)
target_compile_definitions(sotb-launcher-ui-tests PRIVATE _CRT_SECURE_NO_WARNINGS)
set_target_properties(sotb-launcher-ui-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
