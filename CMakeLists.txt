cmake_minimum_required(VERSION 3.24)
project(shadow_of_the_beast_pc VERSION 0.2.0 LANGUAGES C CXX)
set(SOTB_PORT_VERSION "${PROJECT_VERSION}")

if (NOT WIN32)
    message(FATAL_ERROR "This round targets Windows x64 only.")
endif()
if (NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Build with Clang: MSYS2 CLANG64 (tools\\build.ps1).")
endif()
if (WIN32)
    enable_language(RC)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fmacro-prefix-map=${CMAKE_SOURCE_DIR}/=>")

if (NOT PROJECT_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    message(FATAL_ERROR "project VERSION must start with MAJOR.MINOR.PATCH")
endif()
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/generated/sotb_version.h CONTENT "#define SOTB_VERSION_NUMBERS \
${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0\n#define SOTB_FILE_VERSION_STRING \
\"${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}.0\"\n#define SOTB_VERSION_STRING \"${PROJECT_VERSION}\"\n")

find_package(SDL3 CONFIG REQUIRED)
find_package(Freetype REQUIRED)
find_package(Threads REQUIRED)
find_package(Zydis REQUIRED)

add_library(sotb_startup_phase STATIC src/startup_phase.c)
target_include_directories(sotb_startup_phase PUBLIC src)
target_compile_options(sotb_startup_phase PRIVATE -Wall -Wextra -Werror)

add_library(sotb_guest_display STATIC src/guest_display.c src/skip_intro.c src/psn_notice.c)
add_library(sotb_resolution_guard STATIC src/resolution_guard.c)
add_library(sotb_exit_game_l10n STATIC src/sotb_exit_game_l10n.c)
target_include_directories(sotb_exit_game_l10n PUBLIC src)
target_compile_options(sotb_exit_game_l10n PRIVATE -Wall -Wextra -Werror)
target_include_directories(sotb_guest_display PUBLIC src)
target_compile_options(sotb_guest_display PRIVATE -Wall -Wextra -Werror)
target_include_directories(sotb_resolution_guard PUBLIC src)
target_link_libraries(sotb_resolution_guard PUBLIC sotb_guest_display sotb_startup_phase)
target_compile_options(sotb_resolution_guard PRIVATE -Wall -Wextra -Werror)

add_library(sotb_keys STATIC src/sotb_keys.c src/sotb_pad.c src/sotb_pad_prompts.c)
target_include_directories(sotb_keys PUBLIC src)
target_link_libraries(sotb_keys PUBLIC SDL3::SDL3)
target_compile_options(sotb_keys PRIVATE -Wall -Wextra -Werror)

add_library(sotb_game_prompts STATIC src/sotb_game_prompts.c src/sotb_prompt_assets.c src/sotb_prompt_assets_data.c)
target_include_directories(sotb_game_prompts PUBLIC src)
target_link_libraries(sotb_game_prompts PUBLIC sotb_keys)
target_compile_options(sotb_game_prompts PRIVATE -Wall -Wextra -Werror)

add_library(sotb_ui_text INTERFACE)
target_include_directories(sotb_ui_text INTERFACE launcher)

set(SOTB_ATRAC9_DIR ${CMAKE_SOURCE_DIR}/third_party/LibAtrac9/C/src)
if (NOT EXISTS ${SOTB_ATRAC9_DIR}/libatrac9.h)
    message(FATAL_ERROR "LibAtrac9 is missing from third_party/LibAtrac9.")
endif()
file(GLOB SOTB_ATRAC9_SOURCES CONFIGURE_DEPENDS ${SOTB_ATRAC9_DIR}/*.c)
add_library(sotb_atrac9 STATIC ${SOTB_ATRAC9_SOURCES})
target_include_directories(sotb_atrac9 PUBLIC ${SOTB_ATRAC9_DIR})
target_compile_options(sotb_atrac9 PRIVATE -w)
set_target_properties(sotb_atrac9 PROPERTIES C_EXTENSIONS ON)

add_subdirectory(gpu)
add_subdirectory(launcher)

set(SOTB_RUNTIME_SOURCES
    src/runtime.c
    src/runtime_ajm.c
    src/runtime_audio.c
    src/runtime_audio3d.c
    src/runtime_content.c
    src/runtime_eventflag.c
    src/runtime_extra.c
    src/runtime_fiber.c
    src/runtime_file.c
    src/runtime_http_uri.c
    src/local_np_store.c
    src/runtime_kernel.c
    src/runtime_memory.c
    src/runtime_mutex.c
    src/runtime_ngs2.c
    src/runtime_np_local.c
    src/runtime_pad.c
    src/runtime_redzone.c
    src/runtime_rtc.c
    src/runtime_rwlock.c
    src/runtime_savedata.c
    src/runtime_sema.c
    src/runtime_services.c
    src/runtime_session_log.c
    src/runtime_thread.c
    src/runtime_time.c)

set(SOTB_GAME_SOURCES
    src/loader.c
    src/crash_dump.c
    src/import_slots.c
    src/guest_patches.c
    src/sotb_exit_game.c
    src/sotb_menu_buttons.c
    src/isa_emulate.c
    src/lzcnt_compat.c
    src/compat_win.c
    src/module_diagnostics.c
    src/module_history.c
    src/call_site.c
    src/runtime_log.c
    ${SOTB_RUNTIME_SOURCES}
    src/shadow_of_the_beast.rc)

add_executable(shadow_of_the_beast WIN32 ${SOTB_GAME_SOURCES})
target_include_directories(shadow_of_the_beast PRIVATE src launcher gpu/shim ${CMAKE_BINARY_DIR}/generated)
target_compile_options(shadow_of_the_beast PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -fno-omit-frame-pointer>)
target_compile_definitions(shadow_of_the_beast PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007)
target_link_options(shadow_of_the_beast PRIVATE -Wl,--disable-high-entropy-va
    "SHELL:-Xlinker --stack -Xlinker 16777216,16777216")
target_link_libraries(shadow_of_the_beast PRIVATE sotb_launcher sotb_game_prompts sotb_exit_game_l10n sotb_resolution_guard sotbgpu sotb_atrac9 SDL3::SDL3 Freetype::Freetype
    onecore bcrypt ws2_32 user32 -pthread)
add_dependencies(shadow_of_the_beast trg-updater)

add_executable(import_slots_test EXCLUDE_FROM_ALL tests/import_slots_test.c src/import_slots.c)
target_include_directories(import_slots_test PRIVATE src)
target_compile_options(import_slots_test PRIVATE -Wall -Wextra -Werror)

add_executable(guest_patches_test EXCLUDE_FROM_ALL tests/guest_patches_tests.c)
target_include_directories(guest_patches_test PRIVATE src)
target_compile_definitions(guest_patches_test PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007)
target_compile_options(guest_patches_test PRIVATE -Wall -Wextra -Werror)

add_executable(ngs2_test EXCLUDE_FROM_ALL tests/ngs2_test.c src/runtime_ngs2.c)
target_include_directories(ngs2_test PRIVATE src)
target_compile_options(ngs2_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(ngs2_test PRIVATE -pthread)

add_executable(runtime_services_test EXCLUDE_FROM_ALL tests/runtime_services_test.c tests/runtime_test_stubs.c
    ${SOTB_RUNTIME_SOURCES} src/compat_win.c src/runtime_log.c src/isa_emulate.c)
target_include_directories(runtime_services_test PRIVATE src tests launcher gpu/shim ${CMAKE_BINARY_DIR}/generated)
target_compile_options(runtime_services_test PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -fno-omit-frame-pointer>)
target_compile_definitions(runtime_services_test PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007)
target_link_libraries(runtime_services_test PRIVATE sotb_startup_phase sotb_keys sotb_atrac9 SDL3::SDL3 Zydis::Zydis onecore bcrypt
    ws2_32 -pthread)

add_executable(memory_windows_test EXCLUDE_FROM_ALL tests/memory_windows_tests.c src/compat_win.c src/runtime_file.c)
target_include_directories(memory_windows_test PRIVATE src)
target_compile_definitions(memory_windows_test PRIVATE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007)
target_compile_options(memory_windows_test PRIVATE -Wall -Wextra -Werror)
target_link_options(memory_windows_test PRIVATE -Wl,--disable-high-entropy-va)
target_link_libraries(memory_windows_test PRIVATE onecore -pthread)

set(SOTB_TEST_DEFINITIONS _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007)
add_executable(runtime_sync_test EXCLUDE_FROM_ALL tests/runtime_tests.c tests/runtime_lookup_stub.c src/runtime_mutex.c src/compat_win.c)
target_compile_definitions(runtime_sync_test PRIVATE SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_executable(savedata_test EXCLUDE_FROM_ALL tests/savedata_tests.c src/runtime_file.c src/compat_win.c)
target_compile_definitions(savedata_test PRIVATE SOTB_SAVEDATA_TEST_ROOT="${CMAKE_BINARY_DIR}/savedata-tests"
    SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_executable(sotb-prompt-overlay-tests EXCLUDE_FROM_ALL tests/runtime_overlay_tests.c src/compat_win.c)
add_executable(module_history_test EXCLUDE_FROM_ALL tests/module_history_tests.c src/module_history.c src/call_site.c
    src/startup_phase.c)
add_executable(log_test EXCLUDE_FROM_ALL tests/log_tests.c src/runtime_log.c src/runtime_session_log.c)
add_executable(ajm_test EXCLUDE_FROM_ALL tests/ajm_tests.c)
target_link_libraries(ajm_test PRIVATE sotb_atrac9 -pthread)
foreach (test IN ITEMS runtime_sync_test savedata_test sotb-prompt-overlay-tests module_history_test log_test ajm_test)
    target_include_directories(${test} PRIVATE src tests launcher gpu/shim ${CMAKE_BINARY_DIR}/generated)
    target_compile_definitions(${test} PRIVATE ${SOTB_TEST_DEFINITIONS})
    target_compile_options(${test} PRIVATE -Wall -Wextra)
endforeach()
target_link_libraries(runtime_sync_test PRIVATE onecore bcrypt -pthread)
target_link_libraries(savedata_test PRIVATE onecore bcrypt -pthread)
target_link_libraries(sotb-prompt-overlay-tests PRIVATE onecore bcrypt -pthread)
target_link_libraries(module_history_test PRIVATE -pthread)
target_link_libraries(log_test PRIVATE -pthread)

add_executable(avplayer_thread_test EXCLUDE_FROM_ALL tests/avplayer_thread_tests.cpp)
set_target_properties(avplayer_thread_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(avplayer_thread_test PRIVATE gpu/shadps4)
target_compile_options(avplayer_thread_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(avplayer_thread_test PRIVATE -pthread)

add_custom_command(TARGET shadow_of_the_beast POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:shadow_of_the_beast>/assets/launcher"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/header-art.png"
        "$<TARGET_FILE_DIR:shadow_of_the_beast>/assets/launcher/header-art.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/launcher/sponsor-heart.png"
        "$<TARGET_FILE_DIR:shadow_of_the_beast>/assets/launcher/sponsor-heart.png"
    VERBATIM)

set_source_files_properties(src/shadow_of_the_beast.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/packaging/icons/app-icon.ico;${CMAKE_SOURCE_DIR}/src/shadow_of_the_beast.manifest;${CMAKE_BINARY_DIR}/generated/sotb_version.h")

add_executable(image_descriptor_test EXCLUDE_FROM_ALL tests/image_descriptor_tests.cpp gpu/shadps4/video_core/amdgpu/pixel_format.cpp
    gpu/shadps4/video_core/amdgpu/tiling.cpp)
set_target_properties(image_descriptor_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(image_descriptor_test PRIVATE $<TARGET_PROPERTY:sotbgpu,INCLUDE_DIRECTORIES>)
target_compile_definitions(image_descriptor_test PRIVATE $<TARGET_PROPERTY:sotbgpu,COMPILE_DEFINITIONS>)
target_compile_options(image_descriptor_test PRIVATE -Wall -Wextra)
target_link_libraries(image_descriptor_test PRIVATE fmt::fmt)

add_executable(hdr_format_test EXCLUDE_FROM_ALL tests/hdr_format_tests.cpp gpu/shadps4/video_core/amdgpu/pixel_format.cpp gpu/shadps4/common/number_utils.cpp)
set_target_properties(hdr_format_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(hdr_format_test PRIVATE $<TARGET_PROPERTY:sotbgpu,INCLUDE_DIRECTORIES>)
target_compile_definitions(hdr_format_test PRIVATE $<TARGET_PROPERTY:sotbgpu,COMPILE_DEFINITIONS>)
target_compile_options(hdr_format_test PRIVATE -Wall -Wextra)
target_link_libraries(hdr_format_test PRIVATE fmt::fmt)

add_executable(blend_min_max_test EXCLUDE_FROM_ALL tests/blend_min_max_tests.cpp)
set_target_properties(blend_min_max_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(blend_min_max_test PRIVATE $<TARGET_PROPERTY:sotbgpu,INCLUDE_DIRECTORIES>)
target_compile_definitions(blend_min_max_test PRIVATE $<TARGET_PROPERTY:sotbgpu,COMPILE_DEFINITIONS>)
target_compile_options(blend_min_max_test PRIVATE -Wall -Wextra)

add_executable(identity_lut_test EXCLUDE_FROM_ALL tests/identity_lut_tests.cpp)
set_target_properties(identity_lut_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(identity_lut_test PRIVATE gpu/shadps4)
target_compile_options(identity_lut_test PRIVATE -Wall -Wextra -Werror)

add_executable(sotb-window-tests EXCLUDE_FROM_ALL tests/window_fit_tests.cpp)
set_target_properties(sotb-window-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sotb-window-tests PRIVATE src)
target_compile_options(sotb-window-tests PRIVATE -Wall -Wextra -Werror)

add_executable(sotb-renderer-selection-tests EXCLUDE_FROM_ALL tests/renderer_selection_tests.cpp)
set_target_properties(sotb-renderer-selection-tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sotb-renderer-selection-tests PRIVATE gpu/shim gpu/shadps4)
target_compile_definitions(sotb-renderer-selection-tests PRIVATE SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(sotb-renderer-selection-tests PRIVATE -Wall -Wextra -Werror)

add_executable(sotb-vulkan-preflight-tests EXCLUDE_FROM_ALL tests/vulkan_preflight_tests.cpp)
set_target_properties(sotb-vulkan-preflight-tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sotb-vulkan-preflight-tests PRIVATE gpu/shim gpu/shadps4)
target_compile_definitions(sotb-vulkan-preflight-tests PRIVATE SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(sotb-vulkan-preflight-tests PRIVATE -Wall -Wextra -Werror)

add_executable(sotb-buffer-arena-tests EXCLUDE_FROM_ALL tests/buffer_arena_tests.cpp)
set_target_properties(sotb-buffer-arena-tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sotb-buffer-arena-tests PRIVATE gpu/shadps4)
target_compile_definitions(sotb-buffer-arena-tests PRIVATE SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(sotb-buffer-arena-tests PRIVATE -Wall -Wextra -Werror)

add_executable(sotb-pipeline-cache-tests EXCLUDE_FROM_ALL tests/pipeline_cache_tests.cpp)
set_target_properties(sotb-pipeline-cache-tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sotb-pipeline-cache-tests PRIVATE $<TARGET_PROPERTY:sotbgpu,INCLUDE_DIRECTORIES>)
target_compile_definitions(sotb-pipeline-cache-tests PRIVATE $<TARGET_PROPERTY:sotbgpu,COMPILE_DEFINITIONS> SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(sotb-pipeline-cache-tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(sotb-pipeline-cache-tests PRIVATE fmt::fmt)

add_executable(sotb-pipeline-warmup-tests EXCLUDE_FROM_ALL tests/pipeline_warmup_tests.cpp)
set_target_properties(sotb-pipeline-warmup-tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sotb-pipeline-warmup-tests PRIVATE gpu/shim gpu/shadps4)
target_compile_definitions(sotb-pipeline-warmup-tests PRIVATE SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(sotb-pipeline-warmup-tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(sotb-pipeline-warmup-tests PRIVATE -pthread)

add_executable(sotb-shader-cache-tests EXCLUDE_FROM_ALL tests/shader_cache_tests.cpp)
set_target_properties(sotb-shader-cache-tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sotb-shader-cache-tests PRIVATE gpu/shim gpu/shadps4)
target_compile_definitions(sotb-shader-cache-tests PRIVATE SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(sotb-shader-cache-tests PRIVATE -Wall -Wextra -Werror)

add_executable(sotb-audio-backlog-tests EXCLUDE_FROM_ALL tests/audio_backlog_tests.c)
target_include_directories(sotb-audio-backlog-tests PRIVATE src)
target_compile_options(sotb-audio-backlog-tests PRIVATE -Wall -Wextra -Werror)

add_executable(sotb-command-snapshot-tests EXCLUDE_FROM_ALL tests/command_snapshot_tests.cpp)
set_target_properties(sotb-command-snapshot-tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sotb-command-snapshot-tests PRIVATE gpu/shadps4)
target_compile_definitions(sotb-command-snapshot-tests PRIVATE SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(sotb-command-snapshot-tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(sotb-command-snapshot-tests PRIVATE -pthread)

add_executable(sotb-gpu-recording-tests EXCLUDE_FROM_ALL tests/gpu_recording_tests.cpp)
set_target_properties(sotb-gpu-recording-tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sotb-gpu-recording-tests PRIVATE gpu/shadps4)
target_compile_definitions(sotb-gpu-recording-tests PRIVATE SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(sotb-gpu-recording-tests PRIVATE -Wall -Wextra -Werror)

add_executable(sotb-device-lost-tests EXCLUDE_FROM_ALL tests/device_lost_tests.cpp)
set_target_properties(sotb-device-lost-tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sotb-device-lost-tests PRIVATE gpu/shadps4)
target_compile_definitions(sotb-device-lost-tests PRIVATE SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(sotb-device-lost-tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(sotb-device-lost-tests PRIVATE -pthread)

add_executable(sotb-gpu-crash-timeline-tests EXCLUDE_FROM_ALL tests/gpu_crash_timeline_tests.cpp)
set_target_properties(sotb-gpu-crash-timeline-tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sotb-gpu-crash-timeline-tests PRIVATE gpu/shadps4)
target_compile_definitions(sotb-gpu-crash-timeline-tests PRIVATE SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(sotb-gpu-crash-timeline-tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(sotb-gpu-crash-timeline-tests PRIVATE -pthread)

add_executable(sotb-swapchain-state-tests EXCLUDE_FROM_ALL tests/swapchain_state_tests.cpp)
set_target_properties(sotb-swapchain-state-tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sotb-swapchain-state-tests PRIVATE gpu/shim gpu/shadps4)
target_compile_definitions(sotb-swapchain-state-tests PRIVATE SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(sotb-swapchain-state-tests PRIVATE -Wall -Wextra -Werror)

add_executable(sotb-frame-pacing-tests EXCLUDE_FROM_ALL tests/frame_pacing_tests.cpp)
set_target_properties(sotb-frame-pacing-tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sotb-frame-pacing-tests PRIVATE gpu/shim gpu/shadps4)
target_compile_definitions(sotb-frame-pacing-tests PRIVATE SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(sotb-frame-pacing-tests PRIVATE -Wall -Wextra -Werror)

add_executable(sotb-presentation-hardening-tests EXCLUDE_FROM_ALL tests/presentation_hardening_tests.cpp)
set_target_properties(sotb-presentation-hardening-tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sotb-presentation-hardening-tests PRIVATE gpu/shim gpu/shadps4)
target_compile_definitions(sotb-presentation-hardening-tests PRIVATE SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(sotb-presentation-hardening-tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(sotb-presentation-hardening-tests PRIVATE -pthread)

add_executable(sotb-gpu-memory-fallback-tests EXCLUDE_FROM_ALL tests/gpu_memory_fallback_tests.cpp)
set_target_properties(sotb-gpu-memory-fallback-tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sotb-gpu-memory-fallback-tests PRIVATE gpu/shadps4)
target_compile_definitions(sotb-gpu-memory-fallback-tests PRIVATE SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(sotb-gpu-memory-fallback-tests PRIVATE -Wall -Wextra -Werror)

add_executable(sotb-nvidia-hardening-tests EXCLUDE_FROM_ALL tests/nvidia_hardening_tests.cpp)
set_target_properties(sotb-nvidia-hardening-tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(sotb-nvidia-hardening-tests PRIVATE $<TARGET_PROPERTY:sotbgpu,INCLUDE_DIRECTORIES>)
target_compile_definitions(sotb-nvidia-hardening-tests PRIVATE $<TARGET_PROPERTY:sotbgpu,COMPILE_DEFINITIONS> SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(sotb-nvidia-hardening-tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(sotb-nvidia-hardening-tests PRIVATE fmt::fmt)

add_executable(sotb-isa-tests EXCLUDE_FROM_ALL tests/isa_tests.c src/isa_emulate.c src/lzcnt_compat.c)
target_include_directories(sotb-isa-tests PRIVATE src)
target_compile_definitions(sotb-isa-tests PRIVATE SOTB_BOOT_IMAGE="${CMAKE_SOURCE_DIR}/build/image/CUSA03762-1.00/boot.bin")
target_compile_options(sotb-isa-tests PRIVATE -Wall -Wextra -Werror)

add_executable(sotb-controller-prompts-tests EXCLUDE_FROM_ALL tests/controller_prompts_tests.cpp)
target_link_libraries(sotb-controller-prompts-tests PRIVATE sotb_game_prompts SDL3::SDL3)
target_compile_definitions(sotb-controller-prompts-tests PRIVATE SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(sotb-controller-prompts-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(sotb-controller-prompts-tests PRIVATE -Wall -Wextra)

add_executable(skip_intro_test EXCLUDE_FROM_ALL tests/skip_intro_tests.c)
target_include_directories(skip_intro_test PRIVATE src)
target_compile_definitions(skip_intro_test PRIVATE SOTB_IMAGE_ROOT="${CMAKE_SOURCE_DIR}/build/image" SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(skip_intro_test PRIVATE -Wall -Wextra -Werror)

add_executable(psn_notice_test EXCLUDE_FROM_ALL tests/psn_notice_tests.c)
target_include_directories(psn_notice_test PRIVATE src)
target_compile_definitions(psn_notice_test PRIVATE SOTB_IMAGE_ROOT="${CMAKE_SOURCE_DIR}/build/image" SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_compile_options(psn_notice_test PRIVATE -Wall -Wextra -Werror)

add_executable(sotb-menu-buttons-tests EXCLUDE_FROM_ALL tests/menu_buttons_tests.c)
target_include_directories(sotb-menu-buttons-tests PRIVATE src)
target_link_libraries(sotb-menu-buttons-tests PRIVATE sotb_keys SDL3::SDL3)
target_compile_definitions(sotb-menu-buttons-tests PRIVATE SOTB_BOOT_IMAGE="${CMAKE_SOURCE_DIR}/build/image/CUSA03762-1.00/boot.bin" SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(sotb-menu-buttons-tests PRIVATE -Wall -Wextra -Werror)

add_executable(sotb-title-bar-tests EXCLUDE_FROM_ALL tests/title_bar_tests.cpp)
target_include_directories(sotb-title-bar-tests PRIVATE gpu/shim src)
target_compile_definitions(sotb-title-bar-tests PRIVATE SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(sotb-title-bar-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(sotb-title-bar-tests PRIVATE -Wall -Wextra -Werror)

add_executable(sotb-exit-game-tests EXCLUDE_FROM_ALL tests/exit_game_tests.cpp src/sotb_exit_game.c src/sotb_menu_buttons.c)
target_include_directories(sotb-exit-game-tests PRIVATE src gpu/shim launcher)
target_link_libraries(sotb-exit-game-tests PRIVATE sotb_exit_game_l10n sotb_keys)
target_compile_definitions(sotb-exit-game-tests PRIVATE SOTB_BOOT_IMAGE="${CMAKE_SOURCE_DIR}/build/image/CUSA03762-1.00/boot.bin" SOTB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(sotb-exit-game-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(sotb-exit-game-tests PRIVATE -Wall -Wextra -Werror)

add_executable(sotb-loading-hint-layout-tests EXCLUDE_FROM_ALL tests/loading_hint_layout_tests.cpp src/guest_display.c)
target_include_directories(sotb-loading-hint-layout-tests PRIVATE src)
set_target_properties(sotb-loading-hint-layout-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(sotb-loading-hint-layout-tests PRIVATE -Wall -Wextra -Werror)

add_executable(sotb-resolution-guard-tests EXCLUDE_FROM_ALL tests/resolution_guard_tests.cpp)
target_include_directories(sotb-resolution-guard-tests PRIVATE src launcher)
target_link_libraries(sotb-resolution-guard-tests PRIVATE sotb_resolution_guard)
set_target_properties(sotb-resolution-guard-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(sotb-resolution-guard-tests PRIVATE -Wall -Wextra -Werror)
