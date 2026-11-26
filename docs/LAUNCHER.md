# Launcher

The launcher is TRG Launcher 1.1.0 with eight pages, in this order: **Play**, **Display**, **Graphics**, **Gameplay**,
**Controls**, **Trophies**, **Bug Report** and **About**. Every option on a page has a key in `user\sotb.ini` and a reader in the
runtime or the video core; `docs/LAUNCHER_PARITY_AUDIT.md` lists each one with its reader and how it was checked.

Settings are saved when you press Play (or Save). Options that need a restart say so. Lines of `sotb.ini` the launcher does
not manage, and comments, are kept. Shadow of the Beast is offline: there is no Online page, no Editor and no Cheats.

## Play

- **Install game...** / **Reinstall game...** opens a file dialog for a PS4 `.pkg`.
- Before unpacking anything, the launcher reads `param.sfo` and checks TITLE_ID (`CUSA03762`, `CUSA02871` or `CUSA00951`),
  the content ID, CATEGORY `gd`, APP_VER `01.00` and VERSION `01.01`.
- The package is unpacked into `game\prepared\<TITLE_ID>-1.00` through a staging folder. The executable hash is checked
  before the staged tree is activated. Each build keeps its own folder; installing or selecting a build makes it active.
- The boot image (`boot.bin`, `content.bin`, `image.json`) is built into `game\image\<TITLE_ID>-1.00`.
- Progress, cancellation and failures are reported on the page; the log has the detailed reason.
- **PLAY** is enabled only when the installed tree, executable hash and boot image all match a supported build.
- **Show this launcher** (`show_launcher`): Off starts the game directly. Hold Shift while starting, or pass `--launcher`.

The game package is only read. Reinstalling replaces the game folder and leaves `user\` and `logs\` untouched.

## Display

| Option | Key | Values |
| --- | --- | --- |
| Display mode | `display_mode` | Windowed, Borderless Fullscreen (F11 switches it in the game) |
| Window size | `window_width`, `window_height` | heights 720, 900, 1080, 1440 and 2160 in the Aspect Ratio's shape (16:9: 1280x720, 1600x900, 1920x1080, 2560x1440, 3840x2160), whatever the monitor. A size that does not fit the monitor is reduced proportionally only when the window is created; the saved value is not changed |
| Monitor | `monitor` | 1-based display |
| Aspect Ratio | `aspect_ratio` | 16:9 Original (default), 16:10, 21:9 Ultrawide, 32:9 Super Ultrawide, 4:3, 5:4; an unknown value is 16:9; applies at the next game start |
| True Ultrawide | `true_ultrawide` | On (default), Off; only for 21:9 and 32:9 (greyed out otherwise). On opens the 3D view sideways (Hor+) in gameplay and on the title and lays the game's widgets out in a centered 16:9 area while it does; off keeps the plain Aspect Ratio; applies at the next game start |
| Renderer | `renderer` | Vulkan (default), Auto, OpenGL (experimental), Direct3D 11 (experimental) |
| Graphics card | `graphics_card` | the name of a usable Vulkan device (no Auto entry; the first usable device when unset) |
| VSync | `vsync` | Off, On |
| Presentation | `presentation` | Letterbox (default), Stretch |
| Upscaling | `fsr1` | Off, FSR 1 |
| Sharpening | `rcas`, `rcas_strength` | Off, On, 0 to 100% |

Auto renderer uses Vulkan and falls back to OpenGL only when this system cannot run Vulkan. A renderer chosen by name
starts or fails as itself. A saved graphics card that is no longer installed is replaced by the first usable one when the launcher opens. FSR 1, Sharpening and
Anti-aliasing run in the Vulkan renderer only.

**Aspect Ratio** sets the shape of the game's render target and of the Window size choices; it does not stretch the 16:9
picture. Changing it moves the saved Window size to the same height in the new shape. The target keeps the Render
resolution's height and takes the width of the shape at that height, aligned to 8 pixels; a target above 3840 x 2160 pixels
loses height in steps of 8 until it fits (32:9 at 4K is 5408 x 1520, never 7680 x 2160). 16:9 Original leaves the Render
resolution as it is. The camera, HUD and prerendered videos are not adjusted: a different shape can show more or less of
the scene or bars on videos. The Resolution Guard judges the real target: one above 2560 x 1440 pixels is guarded, and a
failed startup restores the largest of 1440p, Native, 720p and 480p whose target is not above it for that shape.

## Launcher window

The launcher draws at 60 frames a second whatever the display's refresh rate, paced by a monotonic deadline; a minimized
window sleeps and keeps its events and background work. On Windows it uses Direct3D 11 and falls back to SDL's software
renderer when Direct3D 11 is unavailable; `d3d11.dll` and `dxgi.dll` are loaded once and pinned for the life of the process
because SDL unloads `d3d11.dll` right after the device's last release while a driver thread may still be returning through
it. Elsewhere SDL's first working renderer is used, with software as the fallback. Each renderer is tried once: an attempt
opens its own hidden window, creates the renderer, uploads a texture, clears, draws and presents a first frame and starts the
ImGui backends; a step that fails releases what that attempt opened and the next renderer is tried. When none opens, a
message box of the system lists each renderer and why it failed, and the game is not started. VSync stays on only where
the display really paces; a software renderer's simulated VSync is turned off and the 60 Hz cap alone paces the loop. The
log has `renderer_attempt` (stage `window`, `pin`, `create`, `first_frame` or `imgui_backend`, bounded reason),
`renderer_selected` (with `fallback_reason`), `frame_pacing` and `loop_sample` lines with the requested and effective
renderer, fallback, requested and effective VSync, pacing mode and target Hz; a present that stalls is reported once per
incident.

The launcher window needs no Vulkan. The Graphics card list is asked from Vulkan only after the visible launcher has drawn
at least three frames over half a second; until then the row shows the saved choice, and a system where Vulkan is missing,
too old or finds no card shows that no card was found and keeps every saved setting. The Renderer setting is the game's
and is separate from the launcher's own renderer.

**Software recovery.** `SOTB_LAUNCHER_RENDERER=software` in the environment of one start opens the launcher with SDL's
software renderer and skips the Graphics card detection, so the settings can be reached when the GPU driver misbehaves. It
is never saved, and the game does not read it.

In a window too short for every page, only the sidebar's page list scrolls, with the launcher's usual scrollbar in place of
its right padding (`gpu/patches/trg-launcher/0020-sidebar-scroll.patch`). The wheel over the sidebar scrolls it, the rows are
keyboard and controller targets that ImGui keeps in view, and the current page is brought into view when it changes or the
window is resized; otherwise the scroll stays where the player left it. With every page in view there is no scrollbar.

## Graphics

**Render resolution** (`render_resolution`): Native — 1920 × 1080, 480p — 854 × 480, 720p — 1280 × 720,
1440p — 2560 × 1440, 1800p — 3200 × 1800, 4K — 3840 × 2160. It sets the size of the game's own picture,
not the window; the picture is scaled to the window. It needs the guest patches in `docs/GRAPHICS.md`; if the build
does not match them the game stops with a message instead of ignoring the setting.
**Anti-aliasing** (`smaa`: Off, SMAA) and **Texture filtering** (`anisotropy`: Game, 2x, 4x, 8x, 16x).

## Gameplay

**Menu & Subtitles** (`text_language`; restart; English (US) and English (UK) first, then the other languages alphabetically), **Skip Intro** (`skip_intro`, Off by default; restart), **Frame rate** (`frame_rate`: 30, 60, 90, 120, 144, 165, 240, Unlimited),
**Frame rate in title**, **Render resolution in title**, **Frame counter** (`show_fps`; F2 switches it for the session).
The game's logic stays at its own 60 Hz tick; a higher frame rate shows the last frame again between two game flips.
The frame counter is drawn top right at 100%; its corner and size are not launcher options.

## Controls

Controller detection, **Controller Rumble** (`controller_rumble`), **Controller prompts** (`controller_prompts`: `auto`, `xbox`
or `playstation`; Auto follows the controller connected when the game starts, anything it cannot classify shows Xbox; it sets
the button names on this page and the game's own button icons from the next game start, and never changes a binding; see
`docs/CONTROLLER_PROMPTS.md`), **Menu Button Layout** (`menu_button_layout`: `western` or `original`, default
`western`; Western uses Cross/A to confirm and Circle/B to go back, Original keeps each build's original layout
(Circle/B to confirm on Korea, Cross/A to confirm on USA and Europe); combat controls are unchanged; see
`docs/MENU_BUTTONS.md`), **Controller
bindings** (`pad_cross`, `pad_circle`, ... one SDL gamepad name each: click a row, press a controller button or pull a trigger, Esc cancels; one
controller button may press several PS4 buttons; sticks stay analog; **Reset to defaults** touches only these), keyboard
bindings (click a key, press the new one, Esc cancels, a key may serve more than one control, **Reset to defaults**), the port
hotkeys, and the default controller layout.
`SOTB_PIPELINE_ON_DEMAND=1` keeps pipelines out of the background warm-up. A controller SDL does not recognise needs a line in `user\gamecontrollerdb.txt`; `SDL_GAMECONTROLLERCONFIG`
keeps priority and an unmapped joystick is logged once per connection with its GUID.

## Trophies

See `docs/TROPHIES.md`.

## Bug Report

The page reports a problem with one privacy-filtered game log, with a description. Nothing is sent until **Send report**
is pressed: opening the page, **Preview**, **View selected log**, **Play** and the launcher's start send nothing. The report
service is a Cloudflare Worker of its own, [server/shadow-bug-reports](../server/shadow-bug-reports/README.md), shared with no
other project and independent of the game's offline online-service stubs.

1. **Report the problem**: 10 to 500 characters, with a counter.
2. **Choose one game log**: the newest five `logs\shadow_of_the_beast_NNN.log`. Each is labelled **Current launcher session**,
   **Last gameplay**, **Gameplay session** or **Launcher session**. A session that closed in the launcher without starting the
   game is left out. The default is the newest session that started the game, not the launcher's own log, and the last
   gameplay stays in the list when newer launcher sessions fill it. **View selected log** opens the file in the system's viewer
   and changes nothing.
3. **Preview**: the exact report that **Send report** builds.
4. **Send**: **Save report** writes the same text to `logs\reports\`; the GitHub issue tracker stays available.

What is sent is exactly the preview: description, port version, game build, region, game version, title ID, executable
SHA-256, operating system, CPU, selected and active renderer, graphics card, driver, render resolution, the log's file name,
whether the log was clipped, and the log text. The renderer, CPU, graphics card, driver and resolution are read from the start
of the selected log. The log is read from its last 256 KiB, filtered, cut to its last 64 KiB starting on a whole line, and
sent as UTF-8. No saves, trophies, game files, settings files, screenshots or other logs are sent.

The filter removes or masks Windows and POSIX user names, computer names, home and profile folders, other local folders,
network shares, IPv4 and IPv6 addresses, email addresses, account numbers, tokens, passwords and other labelled secrets,
authorization and cookie values, and control characters. It keeps versions, the renderer, graphics card and driver, guest
addresses, offsets, RVAs, exceptions, stack traces, the build fingerprint, graphics warnings and file names. Automatic filtering
cannot promise to catch everything, which is why the page offers the preview.

The service answers a report with a ticket (`SOB-XXXXXX`) and a private receipt (32 random bytes in hex). The receipt is kept
only in `user\bug_reports.json` and is sent only to ask about that one report; it is never logged or shown. A limit answer
(`429`) shows its `Retry-After` when the service sends one. Each draft carries a random submission key, so pressing **Send
report** again after an answer that never arrived returns the report already filed instead of a second one.

### Crashes, GPU device loss and crash dumps

Each finished session in the list says how it ended, from the lines its own exit path wrote: **Crash detected** (the crash
handler's report), **GPU device lost** (the first device-lost report, which ends the game with exit code 25), **Stopped by
an error**, **Could not start** or **Unsupported game build**. A normal exit, a quit in the launcher and an update restart are
never shown as a failure. The report sends how the session ended, the line that names the failure (for a device loss: the
Vulkan operation, result, stage, GPU, driver and fault summary) and the last startup stage; the GPU timeline stays in the log.

When the game crashes it writes `logs\shadow_of_the_beast_NNN.dmp` beside the session's log (thread stacks and module lists,
no heap and no environment block) and names it in that log; the newest five dumps are kept. For the selected log the page
offers **Attach crash report**, on by default, only for that session's own dump: exactly that name in the same folder, a
regular file the log names, written within two minutes of the log's end, 1 KiB to 8 MiB and starting with `MDMP`. A dump
that fails a check is never sent and the page says why; the report goes without it. The dump is uploaded after the report in
1 MiB chunks under the report's receipt and resumes from what the service already holds. If it does not arrive, the report
stays sent and **Retry crash report upload** sends only the dump again, under the same Report ID. Only the dump's file name,
size and SHA-256 are in the report; no folder of the PC is sent.

### My Reports

The reports sent from this PC (the newest 30) are listed under the form. The launcher asks the service about them off the UI
thread when it starts, when the page is entered (unless a check ran in the last ten seconds), every three minutes while the
page stays open, and on **Refresh**; one check at a time, and nothing is asked when the list is empty. A failed check keeps
the last known status and message. The page's first visit shows **Loading Bug Report...** while the game logs are listed off
the UI thread.

- A changed status marks the report **Updated**. A developer message that is new, changed, or saved again as a new revision is
  a **New reply**: the row shows a New reply marker, the Bug Report item in the sidebar shows its dot, and the page shows "A
  report has a new reply" with **Show**. A reply comes before a plain update, and reports with an unread reply are listed
  first. A status change alone is never a reply.
- Opening the page clears nothing. A report counts as seen only when it is opened, which also shows the **Message from
  developer** panel.
- When the service answers a status check without a report that was asked about, the report is marked **No longer available**.
  Its last known status and message stay, it stops counting as an update, and **Refresh** asks again. **Remove from this PC**
  deletes only the local entry; a damaged `bug_reports.json` is set aside as `bug_reports.json.damaged`.

Failures never block the launcher or **Play**: requests run on their own threads with timeouts of 8 s to connect, 15 s to send,
20 s to receive and 30 s in all, are cancelled between steps when the launcher closes, and end in a plain message.

### Player tests

When the developer sets a report to **Awaiting player test**, the message from the developer says what to test and the report
asks **Did this fix the issue?** with **Yes, fixed** and **No, still happening**. The answer is sent with the report's receipt,
the port version and the game build, for that one request (the status time it was shown with); a newer request asks again,
and an answer to an older one is refused. The first answer to a request is final. The answer never changes the status: the
developer sets **Fixed** or anything else. After **No, still happening** the report stays as it is and **Send a new report**
returns to the form above, which sends nothing until **Send report** is pressed, so the new report carries a current log.

### Service address

A normal launcher sends to `https://shadow-bug-reports.saulobenigno.workers.dev` (`kBugReportServiceUrl` in
`launcher/game_profile.h`) with no setting. `SOTB_BUG_REPORT_URL` is only an override for development and tests, for one run
(`https://host[:port]`, https only); an invalid value disables sending for that run instead of falling back. Opening the
page contacts nothing. The developer dashboard is `https://shadow-bug-reports.saulobenigno.workers.dev/admin`, protected by the Worker secret
`SOTB_BUG_REPORT_ADMIN_TOKEN`; see the service's README for its endpoints, limits and retention. `.\tools\worker.ps1 -DryRun` validates and bundles the Worker without uploading, `.\tools\worker.ps1` deploys it, and `namespace_id` in `wrangler.toml` and the admin secret are set by hand first.

## Updates

When the launcher opens it asks GitHub for the latest published release of `saulob/shadow-of-the-beast-pc` (a plain GET; nothing about
the PC is sent). A draft, a prerelease or a version that is not newer is ignored. When the automatic check fails (offline, rate
limited, no release yet) the launcher stays quiet and writes the reason to the log; Play is never blocked by it.

A newer release shows a banner. **UPDATE NOW** downloads the package, and nothing is downloaded or installed before that click.
The package is checked, unpacked into a temporary workspace and validated; **RESTART TO UPDATE** then closes the launcher and
`trg-updater.exe` replaces the program files and starts the new version at the launcher. A failed step shows **RETRY**; it
runs only when pressed, starts that step again (check, download or install) from the beginning and verifies everything again.

| Contract | Value |
| --- | --- |
| Release address | `https://api.github.com/repos/saulob/shadow-of-the-beast-pc/releases/latest` |
| Package | `ShadowOfTheBeastPC-v<version>-windows-x64.zip` |
| Checksum | `ShadowOfTheBeastPC-v<version>-windows-x64.zip.sha256` (`<64 hex digits>  <package name>`) |
| Required in the package | `shadow_of_the_beast.exe`, `trg-updater.exe`, `SDL3.dll`, `libc++.dll` |
| Version | `project(... VERSION x.y.z)` in `CMakeLists.txt`, the only source |

Every request and redirect stays on HTTPS to `api.github.com`, `github.com`, `objects.githubusercontent.com` and
`release-assets.githubusercontent.com`; a package or checksum address must also start with
`https://github.com/saulob/shadow-of-the-beast-pc/releases/download/`. The downloaded size must equal the published size and the
SHA-256 must equal the digest GitHub publishes for the package, or else the one in the checksum file (which must match its own
published digest when the release has one). A release with neither is refused. Downloads go through `<file>.part`.

Only the files of the package are replaced, each one backed up first and compared with its source afterwards; any failure puts the
backups back, and a restart that fails restores the previous version. Nothing else is removed. A package that contains `game\`,
`user\`, `logs\`, `cache\`, `shader\`, saves, trophies, crash dumps, `sotb.ini`, `keys.json`, `bug_reports.json`, anything named
`CUSA03762*`, `CUSA02871*` or `CUSA00951*`, `.pak`, `.pkg` or `eboot.bin` is refused. The helper writes `trg-updater.log` in the
update workspace (`trg-update-*` in the temporary folder).

`.\tools\package.ps1` builds and writes the release locally (nothing is published): `dist\ShadowOfTheBeastPC-v<version>-windows-x64.zip`,
its `.zip.sha256` and the staged folder, checked first for game and user data, logs, dumps, debug files, local paths and
unexpected files. `.\tools\package.ps1 -VerifyOnly <staged folder>` repeats only that check.

Test mode never turns on by itself. With `SOTB_UPDATE_TEST=1`, `SOTB_UPDATE_TEST_CURRENT_VERSION`, `SOTB_UPDATE_TEST_RELEASE_JSON`
(a release file instead of GitHub) and `SOTB_UPDATE_TEST_ASSET_SHA256` can simulate a newer release; the restarted program
does not inherit them. The updater tests use a loopback server and temporary folders only: `sotb-update-tests`,
`sotb-update-network-tests` and `sotb-update-helper-tests`.

## About

Port version, installed build, renderer, folders, the logs folder, **Detailed logging** (`detailed_logging`), the settings
file, **Reset all settings** and the sponsor link. Reset puts only the managed settings back to their defaults; the
launcher window's placement, saves, scores, trophies, the installed game, logs and caches are kept.

## Files

| Path | Contents |
| --- | --- |
| `game\prepared\CUSA03762-1.00\` | installed game (unpacked from your package) |
| `game\image\CUSA03762-1.00\` | boot image built from the installed game |
| `user\sotb.ini` | launcher and runtime settings |
| `user\keys.json`, `user\trophy\` | Trophy Key and trophy progress |
| `user\bug_reports.json` | the reports sent from this PC, with their receipts |
| `user\gamecontrollerdb.txt` | optional controller mappings |
| `user\savedata\` | save data (created by the runtime when the game writes) |
| `logs\shadow_of_the_beast_NNN.log` | one log per run; the log opens with `Settings:` lines |

Nothing is written to the repository by a player installation. `game\`, `user\`, `logs\` and `build\` are ignored by Git.

## Failure messages

An unsupported package is refused with a diagnostic block written to the log. The block lists the package's
`param.sfo`, the `eboot.bin` hash, the required files that were found or missing, and the supported build. This is
intentional: a different executable can have different code, and the port must not run it with the wrong expectations.

The block starts with `Build verdict:` `SUPPORTED`, `UNSUPPORTED` or `POSSIBLE CANDIDATE`, then lists TITLE_ID, CONTENT_ID,
APP_VER, VERSION, the executable's size, entry point, image extent, SHA-256 and code and data ranges, and for the closest
supported build which of those fields match, differ or were not recorded. A candidate is a lead for maintainers and is never
run; only an exact match of every recorded field is supported (`launcher/build_identity.cpp`). The loader prints the same
verdict, from the boot image report, before it touches the guest.
