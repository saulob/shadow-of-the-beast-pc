# Launcher parity audit

Reference: the Dragon's Crown Pro PC launcher. Status: FUNCTIONAL (a reader changes behaviour), PARTIAL (reader exists but
not on every path), PLACEBO (saved or shown, nothing reads it), MISSING. Test class: H = host headless (`sotb-launcher-tests`,
`sotb-trophy-tests`, `runtime_services_test`), U = needs a run by the player.

| Option | Key | Reader | Status before | Action | Test |
| --- | --- | --- | --- | --- | --- |
| Show this launcher | `show_launcher` | `trg::ShouldShowLauncher` | FUNCTIONAL | kept | H settings, U startup |
| Display mode | `display_mode` | `gpu/shim/window.cpp`, F11 writes it back | FUNCTIONAL | kept | H parse, U |
| Window size | `window_width/height` | `window.cpp` (`requested_*`) | FUNCTIONAL | kept | H parse, U |
| Monitor | `monitor` | `window.cpp` | FUNCTIONAL | kept | H parse, U |
| Renderer | `renderer` | `sotbgpu.cpp` Resolve | PARTIAL: the launcher defaulted to Auto while the runtime ran Vulkan | default is Vulkan in both | H selection, Auto fallback, parse; U per renderer |
| Graphics card | `graphics_card` | `vk_instance.cpp` ResolveGpuByName | FUNCTIONAL | no Auto entry (Dragon semantics: GpuChoices, ResolveGpuByName); a missing saved card is normalized to the first usable one | H empty, one, many, removed; U |
| VSync | `vsync` | `window.cpp`, swapchains, `driver.cpp` | FUNCTIONAL | kept | H parse, U |
| Presentation | `presentation` | `vk_presenter`, `gl_presenter` | FUNCTIONAL | kept | H parse, U |
| Upscaling FSR 1 | `fsr1` | `vk_presenter` only | PARTIAL | help says Vulkan only | H `FsrPassRuns`, U |
| Sharpening, strength | `rcas`, `rcas_strength` | `vk_presenter` only | PARTIAL | help says Vulkan only | H, U |
| Anti-aliasing SMAA | `smaa` | `vk_presenter` only | PARTIAL | help says Vulkan only | H parse, U |
| Texture filtering | `anisotropy` | Vulkan and OpenGL rasterizers | FUNCTIONAL | kept | H parse, U |
| Render resolution | `render_resolution` | `src/guest_display.c` display size sites, fingerprinted | PLACEBO (one choice, Native) | six choices, patches the game's display size | H targets and fingerprints, U |
| Frame rate | `frame_rate` | `videoout/driver.cpp` pacing | FUNCTIONAL | kept | H parse and list, U |
| Frame rate in title | `show_fps_title` | `window.cpp` | FUNCTIONAL | kept | H parse, U |
| Render resolution in title | `show_resolution_title` | `window.cpp` | FUNCTIONAL | kept | H parse, U |
| Frame counter | `show_fps` | `sotb_overlay.cpp`, F2 in `window.cpp` | FUNCTIONAL | kept | H parse, U |
| Frame counter size | `overlay_scale` | `sotb_overlay.cpp` | FUNCTIONAL, not a Dragon launcher row | removed from the launcher | H not managed |
| Frame counter corner | `overlay_corner` | `sotb_overlay.cpp` | FUNCTIONAL, not a Dragon launcher row | removed from the launcher; default top right | H not managed |
| Keyboard bindings | per action | `runtime_pad.c` via `sotb_keys` | FUNCTIONAL | kept | H load, invalid, duplicate, Escape unbindable, U |
| Controller Rumble | `controller_rumble` | `runtime_pad.c` (`sotbgpu_controller_rumble`) | MISSING | added | H parse, U |
| Controller prompts | `controller_prompts` | `sotb_game_prompts.c`: the game's button icons, chosen once at startup | MISSING (names on the Controls page only) | Xbox art served over the unchanged pak (`docs/CONTROLLER_PROMPTS.md`) | H `sotb-controller-prompts-tests`, `controller_prompts_assets_tests.py`; U |
| Menu Button Layout | `menu_button_layout` | `sotb_menu_buttons.c`: the game's menu key registration, patched once at startup | MISSING (the Korean `KR_BD` build uses Circle/B to confirm) | Western (default) forces Cross/A confirm; Original keeps the Korean `KR_BD` layout (`docs/MENU_BUTTONS.md`) | H `sotb-menu-buttons-tests`; U |
| gamecontrollerdb, unmapped joystick log | n/a | `runtime_pad.c` | FUNCTIONAL | documented | U |
| Xbox mapping | n/a | `runtime_pad.c` table | MISSING | read-only mapping table on Controls | H table |
| Menu & Subtitles | `text_language` | `runtime_set_language` (system language) | MISSING | added, 19 languages | H table, parse, fallback; U |
| Detailed logging | `detailed_logging` | `runtime_session_log.c` | MISSING (always on) | added | H parse; U |
| Bug Report page, My Reports | `user\bug_reports.json`, `server/shadow-bug-reports` | launcher only (no game code) | MISSING | ported from Odin (the service, the filter, the history); Shadow log format | H `sotb-bug-report-tests`, Worker `npm test`; U |
| Trophies | `user\trophy`, `user\keys.json` | `sceNpTrophy*` in `runtime_services.c` | MISSING (calls returned unavailable) | ported from Dragon | H 388 checks; U |
| Reset all settings | n/a | `LauncherSettings::ResetToDefaults` | PARTIAL (cleared every line, placement included) | managed keys only, placement kept | H |

## Not done

| Item | Reason |
| --- | --- |
| Updater, overlay programs, install changes | not ported |
| Mouse controls | `runtime_pad.c` has no mouse consumer |

## Languages (CUSA03762 1.00, `pakchunk0-ps4.pak` index)

`Beast/Content/Localization/Game/<culture>/Game.locres` exists, uncompressed, 622,618 to 784,456 bytes and different in size
for each of: da, de, en_GB, en_US, es_ES, es_MX, fi, fr, it, ja, ko, nb, nl, pl, pt_BR, pt_PT, ru, sv, zh_Hant. Engine strings
exist for en, es, ja, ko, pt-BR. No per-language voice or subtitle assets appear in any of the nine paks (18,071 entries):
voice is not language specific. The files were not decoded, so the translation quality of each is not verified, and whether
the guest honours each system language id is for the player to check.

## Display and Graphics round

- Display and Graphics rows and order match Dragon, including Render resolution, now backed
  by guest patches (see `docs/GRAPHICS.md`).
- Help texts for Window size, Renderer, Presentation, Upscaling, Sharpening, Anti-aliasing and Texture filtering are Dragon's.
- `renderer_factory.cpp`, `renderer_backend.h` and `sotb_renderer.h` are identical to Dragon after renaming; the OpenGL
  and ANGLE files differ only in message-box titles and a Shadow-specific area-sampling shader.
- Render resolution: the six display-metrics sites were found in the mapped boot image with `objdump`; every size is 16:9.
  Aspect Ratio and wide-screen support were removed from this version.

## Window fit, languages and icons round

- Window size lists five 16:9 sizes whatever the monitor. `src/sotb_window_fit.h` keeps the requested size when the
  decorated window fits the usable area and otherwise shrinks it by one common factor; only the vertical borders limit the
  fit. Borders come from `SDL_GetWindowBordersSize`, else `EstimatedBorders` (40 px at 100%, scaled). The saved size is
  never changed. `sotb-window-tests` covers it.
- Menu & Subtitles order: English (US), English (UK), then alphabetical by English name (`SortTextLanguages`). The
  PS4 system language ids are unchanged.
- The app icon (`packaging/icons/app-icon.*`), the sidebar SVGs and their optical scales are Dragon's.
- Renderer selection is covered by `sotb-renderer-selection-tests`. Direct3D 11 is OpenGL ES 3.0 on ANGLE's Direct3D 11,
  checked through `GL_RENDERER` after the context exists; it is listed on Windows only.
- Help texts for Controller Rumble and Menu & Subtitles are Dragon's. Differing on purpose: Render resolution (semicolon),
  Frame rate, Frame counter and the two title toggles, which describe this game's own behaviour.

## Skip Intro

With Skip Intro On the host presents black (`VideoOutDriver::Flip`, shared by Vulkan, OpenGL and Direct3D 11) while the engine loads, and shows the game again when it calls `ShowStartButton` at 0x1ca6b6 on its own way to Press Start; a time limit of 90 s shows it anyway. The game keeps running and no input is injected. `src/skip_intro.c` also removes the one-second wait before the prompt (0x1ca55c). Both patches are validated first against CUSA03762 1.00; if anything differs nothing is written and the game starts as before.
