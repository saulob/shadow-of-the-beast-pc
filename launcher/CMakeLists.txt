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
    build_identity.cpp
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
    pad_capture.cpp
    page_icons.cpp
    paths.cpp
    pkg_extract.cpp
    report_history.cpp
    resolution_recovery.cpp
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
target_link_libraries(sotb_launcher_core PUBLIC trg_core SDL3::SDL3 sotb_startup_phase sotb_guest_display sotb_resolution_guard
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

add_library(sotb_launcher STATIC host.cpp incident_page.cpp launcher.cpp my_reports_page.cpp trophy_page.cpp update_config.cpp
    updater/github_release.cpp updater/json.cpp updater/sha256.cpp updater/third_party/miniz/miniz.c
    updater/update_controller.cpp updater/update_job.cpp updater/update_paths.cpp updater/update_platform.cpp
    updater/update_ui.cpp updater/update_zip.cpp updater/version.cpp)
set_source_files_properties(updater/third_party/miniz/miniz.c PROPERTIES COMPILE_OPTIONS "-w")
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

add_executable(sotb-aspect-ratio-tests EXCLUDE_FROM_ALL ../tests/aspect_ratio_tests.cpp ../src/guest_patches.c
    ../src/sotb_exit_game.c ../src/sotb_menu_buttons.c)
target_link_libraries(sotb-aspect-ratio-tests PRIVATE sotb_launcher_core sotb_exit_game_l10n sotb_keys SDL3::SDL3)
target_include_directories(sotb-aspect-ratio-tests PRIVATE ${PROJECT_SOURCE_DIR}/gpu/shim ${PROJECT_SOURCE_DIR}/tests)
target_compile_definitions(sotb-aspect-ratio-tests PRIVATE SOTB_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(sotb-aspect-ratio-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(sotb-aspect-ratio-tests PRIVATE -Wall -Wextra)

add_executable(sotb-pad-binding-tests EXCLUDE_FROM_ALL ../tests/pad_binding_tests.cpp)
target_link_libraries(sotb-pad-binding-tests PRIVATE sotb_launcher_core sotb_keys SDL3::SDL3)
target_compile_definitions(sotb-pad-binding-tests PRIVATE SOTB_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(sotb-pad-binding-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(sotb-pad-binding-tests PRIVATE -Wall -Wextra)

add_executable(sotb-pad-prompt-tests EXCLUDE_FROM_ALL ../tests/pad_prompt_tests.cpp)
target_link_libraries(sotb-pad-prompt-tests PRIVATE sotb_launcher_core sotb_keys SDL3::SDL3)
target_compile_definitions(sotb-pad-prompt-tests PRIVATE SOTB_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(sotb-pad-prompt-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(sotb-pad-prompt-tests PRIVATE -Wall -Wextra)

add_executable(sotb-gamepad-database-tests EXCLUDE_FROM_ALL ../tests/gamepad_database_tests.cpp)
target_link_libraries(sotb-gamepad-database-tests PRIVATE SDL3::SDL3)
target_include_directories(sotb-gamepad-database-tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(sotb-gamepad-database-tests PRIVATE SOTB_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(sotb-gamepad-database-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(sotb-gamepad-database-tests PRIVATE -Wall -Wextra)

add_executable(sotb-input-device-tests EXCLUDE_FROM_ALL ../tests/input_device_tests.cpp)
target_include_directories(sotb-input-device-tests PRIVATE ${PROJECT_SOURCE_DIR}/gpu/shim)
set_target_properties(sotb-input-device-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(sotb-input-device-tests PRIVATE -Wall -Wextra)

add_executable(sotb-cursor-policy-tests EXCLUDE_FROM_ALL ../tests/cursor_policy_tests.cpp)
target_include_directories(sotb-cursor-policy-tests PRIVATE ${PROJECT_SOURCE_DIR}/gpu/shim)
target_compile_definitions(sotb-cursor-policy-tests PRIVATE SOTB_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(sotb-cursor-policy-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(sotb-cursor-policy-tests PRIVATE -Wall -Wextra)

foreach (launcher_test IN ITEMS renderer pacing performance)
    add_executable(sotb-launcher-${launcher_test}-tests EXCLUDE_FROM_ALL ../tests/launcher_${launcher_test}_tests.cpp)
    target_include_directories(sotb-launcher-${launcher_test}-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ../tests)
    target_compile_definitions(sotb-launcher-${launcher_test}-tests PRIVATE SOTB_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_target_properties(sotb-launcher-${launcher_test}-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(sotb-launcher-${launcher_test}-tests PRIVATE -Wall -Wextra -Werror)
endforeach()

add_executable(sotb-build-identity-tests EXCLUDE_FROM_ALL ../tests/build_identity_tests.cpp)
target_link_libraries(sotb-build-identity-tests PRIVATE sotb_launcher_core)
target_compile_definitions(sotb-build-identity-tests PRIVATE SOTB_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(sotb-build-identity-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(sotb-build-identity-tests PRIVATE -Wall -Wextra)

add_executable(sotb-bug-report-tests EXCLUDE_FROM_ALL ../tests/bug_report_tests.cpp)
target_link_libraries(sotb-bug-report-tests PRIVATE sotb_launcher_core)
target_compile_definitions(sotb-bug-report-tests PRIVATE SOTB_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(sotb-bug-report-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(sotb-bug-report-tests PRIVATE -Wall -Wextra)

# Crash reports without a window or a network: dump pairing and checks, the resumable upload against a stand-in service,
# device-lost sessions from the monitor's own lines, and the startup stage.
add_executable(sotb-crash-dump-correlation-tests EXCLUDE_FROM_ALL ../tests/crash_dump_correlation_tests.cpp ../src/crash_dump.c
    ../src/runtime_log.c ../src/runtime_session_log.c)
add_executable(sotb-crash-dump-upload-tests EXCLUDE_FROM_ALL ../tests/crash_dump_upload_tests.cpp)
add_executable(sotb-device-lost-reporting-tests EXCLUDE_FROM_ALL ../tests/device_lost_reporting_tests.cpp)
target_include_directories(sotb-device-lost-reporting-tests PRIVATE ${PROJECT_SOURCE_DIR}/gpu/shadps4)
add_executable(sotb-startup-diagnostics-tests EXCLUDE_FROM_ALL ../tests/startup_diagnostics_tests.cpp ../src/module_history.c)
foreach (test IN ITEMS sotb-crash-dump-correlation-tests sotb-crash-dump-upload-tests sotb-device-lost-reporting-tests sotb-startup-diagnostics-tests)
    target_link_libraries(${test} PRIVATE sotb_launcher_core)
    target_include_directories(${test} PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_compile_definitions(${test} PRIVATE SOTB_SOURCE_DIR="${PROJECT_SOURCE_DIR}" _WIN32_WINNT=0x0A00)
    set_target_properties(${test} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(${test} PRIVATE -Wall -Wextra)
endforeach()

add_executable(sotb-launcher-ui-tests EXCLUDE_FROM_ALL ../tests/launcher_ui_tests.cpp ${TRG_DIR}/src/launcher.cpp
    ${TRG_DIR}/src/headers.cpp ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp)
target_link_libraries(sotb-launcher-ui-tests PRIVATE sotb_launcher_core trg_core sotb_keys SDL3::SDL3)
target_include_directories(sotb-launcher-ui-tests PRIVATE ${IMGUI_DIR} ${PROJECT_SOURCE_DIR}/gpu/shim)
target_compile_definitions(sotb-launcher-ui-tests PRIVATE _CRT_SECURE_NO_WARNINGS)
set_target_properties(sotb-launcher-ui-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# Launcher robustness: placement, startup lifecycle, UI stalls, Escape handling and Bug Report feedback, headless.
add_executable(sotb-launcher-placement-tests EXCLUDE_FROM_ALL ../tests/launcher_placement_tests.cpp)
add_executable(sotb-launcher-startup-tests EXCLUDE_FROM_ALL ../tests/launcher_startup_tests.cpp)
add_executable(sotb-launcher-hang-tests EXCLUDE_FROM_ALL ../tests/launcher_hang_tests.cpp)
add_executable(sotb-bug-report-feedback-tests EXCLUDE_FROM_ALL ../tests/bug_report_feedback_tests.cpp)
add_executable(sotb-launcher-navigation-tests EXCLUDE_FROM_ALL ../tests/launcher_navigation_tests.cpp ${TRG_DIR}/src/launcher.cpp
    ${TRG_DIR}/src/headers.cpp ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp)
add_executable(sotb-launcher-sidebar-tests EXCLUDE_FROM_ALL ../tests/launcher_sidebar_tests.cpp ${TRG_DIR}/src/launcher.cpp
    ${TRG_DIR}/src/headers.cpp ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp)
target_link_libraries(sotb-launcher-placement-tests PRIVATE sotb_launcher_core)
target_link_libraries(sotb-launcher-startup-tests PRIVATE sotb_startup_phase)
target_link_libraries(sotb-bug-report-feedback-tests PRIVATE sotb_launcher_core)
target_link_libraries(sotb-launcher-navigation-tests PRIVATE sotb_launcher_core trg_core sotb_keys SDL3::SDL3)
target_include_directories(sotb-launcher-navigation-tests PRIVATE ${IMGUI_DIR} ${PROJECT_SOURCE_DIR}/gpu/shim)
target_compile_definitions(sotb-launcher-navigation-tests PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(sotb-launcher-sidebar-tests PRIVATE sotb_launcher_core trg_core sotb_keys SDL3::SDL3)
target_include_directories(sotb-launcher-sidebar-tests PRIVATE ${IMGUI_DIR} ${PROJECT_SOURCE_DIR}/gpu/shim)
target_compile_definitions(sotb-launcher-sidebar-tests PRIVATE _CRT_SECURE_NO_WARNINGS)
foreach (test IN ITEMS sotb-launcher-placement-tests sotb-launcher-startup-tests sotb-launcher-hang-tests
                       sotb-bug-report-feedback-tests sotb-launcher-navigation-tests sotb-launcher-sidebar-tests)
    target_include_directories(${test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tests)
    target_compile_definitions(${test} PRIVATE SOTB_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_target_properties(${test} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(${test} PRIVATE -Wall -Wextra)
endforeach()

# The helper that replaces the program after it exits. Static, so it needs none of the DLLs it may be replacing.
add_executable(trg-updater WIN32 updater/helper_main.cpp updater/install_transaction.cpp updater/sha256.cpp
    updater/update_job.cpp updater/update_paths.cpp updater/update_platform.cpp)
target_include_directories(trg-updater PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(trg-updater PRIVATE shell32)
target_link_options(trg-updater PRIVATE -static)
set_target_properties(trg-updater PROPERTIES OUTPUT_NAME "trg-updater" CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
target_compile_options(trg-updater PRIVATE -Wall -Wextra)

add_executable(sotb-update-restart-marker EXCLUDE_FROM_ALL WIN32 ../tests/update_restart_marker.cpp)
target_link_libraries(sotb-update-restart-marker PRIVATE shell32)
target_link_options(sotb-update-restart-marker PRIVATE -static)
set_target_properties(sotb-update-restart-marker PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

add_executable(sotb-update-helper-tests EXCLUDE_FROM_ALL ../tests/update_helper_tests.cpp)
target_compile_definitions(sotb-update-helper-tests PRIVATE
    SOTB_UPDATER_EXE="$<TARGET_FILE:trg-updater>"
    SOTB_MARKER_EXE="$<TARGET_FILE:sotb-update-restart-marker>")
add_dependencies(sotb-update-helper-tests trg-updater sotb-update-restart-marker)
set_target_properties(sotb-update-helper-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(sotb-update-helper-tests PRIVATE -Wall -Wextra)

# Versions, release JSON, SHA-256, ZIP safety, staging, jobs, rollback and the release contract; no network.
add_executable(sotb-update-tests EXCLUDE_FROM_ALL ../tests/update_tests.cpp update_config.cpp
    updater/github_release.cpp updater/install_transaction.cpp updater/json.cpp updater/sha256.cpp
    updater/update_job.cpp updater/update_paths.cpp updater/update_platform.cpp updater/update_zip.cpp
    updater/version.cpp updater/third_party/miniz/miniz.c)
target_include_directories(sotb-update-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sotb-update-tests PRIVATE SOTB_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    SOTB_UPDATER_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}" SOTB_PORT_VERSION="${SOTB_PORT_VERSION}")
target_link_libraries(sotb-update-tests PRIVATE trg_core shell32)
set_target_properties(sotb-update-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(sotb-update-tests PRIVATE -Wall -Wextra)

# The update controller against a scripted loopback HTTP server: no release yet (404), 504, dropped connections,
# stalls, redirects, retries, and the production address and asset names. Never contacts GitHub.
add_executable(sotb-update-network-tests EXCLUDE_FROM_ALL ../tests/update_network_tests.cpp update_config.cpp
    updater/github_release.cpp updater/install_transaction.cpp updater/json.cpp updater/sha256.cpp
    updater/update_controller.cpp updater/update_job.cpp updater/update_paths.cpp updater/update_platform.cpp
    updater/update_zip.cpp updater/version.cpp updater/third_party/miniz/miniz.c)
target_include_directories(sotb-update-network-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sotb-update-network-tests PRIVATE SOTB_PORT_VERSION="${SOTB_PORT_VERSION}")
target_link_libraries(sotb-update-network-tests PRIVATE trg_core shell32 ws2_32)
set_target_properties(sotb-update-network-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(sotb-update-network-tests PRIVATE -Wall -Wextra)
