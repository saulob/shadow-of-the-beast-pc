# Launcher

The launcher is TRG Launcher 1.1.0 with seven pages, in this order: **Play**, **Display**, **Graphics**, **Gameplay**,
**Controls**, **Trophies** and **About**. Every option on a page has a key in `user\sotb.ini` and a reader in the
runtime or the video core; `docs/LAUNCHER_PARITY_AUDIT.md` lists each one with its reader and how it was checked.

Settings are saved when you press Play (or Save). Options that need a restart say so. Lines of `sotb.ini` the launcher does
not manage, and comments, are kept. Shadow of the Beast is offline: there is no Online page, no Editor and no Cheats.

## Play

- **Install game...** opens a file dialog for a PS4 `.pkg`.
- Before unpacking anything, the launcher reads `param.sfo` and checks TITLE_ID `CUSA03762`, the content ID, CATEGORY
  `gd`, APP_VER `01.00` and VERSION `01.01`.
- The package is unpacked into `game\prepared\CUSA03762-1.00` through a staging folder. The executable hash is checked
  before the staged tree is activated.
- The boot image (`boot.bin`, `content.bin`, `image.json`) is built into `game\image\CUSA03762-1.00`.
- Progress, cancellation and failures are reported on the page; the log has the detailed reason.
- **PLAY** is enabled only when the installed tree, executable hash and boot image all match the supported build.
- **Show this launcher** (`show_launcher`): Off starts the game directly. Hold Shift while starting, or pass `--launcher`.

The game package is only read. Reinstalling replaces the game folder and leaves `user\` and `logs\` untouched.

## Display

| Option | Key | Values |
| --- | --- | --- |
| Display mode | `display_mode` | Windowed, Borderless Fullscreen (F11 switches it in the game) |
| Window size | `window_width`, `window_height` | 1280x720, 1600x900, 1920x1080, 2560x1440, 3840x2160, whatever the monitor. A size that does not fit the monitor is reduced proportionally only when the window is created; the saved value is not changed |
| Monitor | `monitor` | 1-based display |
| Renderer | `renderer` | Vulkan (default), Auto, OpenGL (experimental), Direct3D 11 (experimental) |
| Graphics card | `graphics_card` | the name of a usable Vulkan device (no Auto entry; the first usable device when unset) |
| VSync | `vsync` | Off, On |
| Presentation | `presentation` | Letterbox (default), Stretch |
| Upscaling | `fsr1` | Off, FSR 1 |
| Sharpening | `rcas`, `rcas_strength` | Off, On, 0 to 100% |

Auto renderer uses Vulkan and falls back to OpenGL only when this system cannot run Vulkan. A renderer chosen by name
starts or fails as itself. A saved graphics card that is no longer installed is replaced by the first usable one when the launcher opens. FSR 1, Sharpening and
Anti-aliasing run in the Vulkan renderer only.

## Graphics

**Render resolution** (`render_resolution`): Native — 1920 × 1080, 480p — 854 × 480, 720p — 1280 × 720,
1440p — 2560 × 1440, 1800p — 3200 × 1800, 4K — 3840 × 2160. It sets the size of the game's own picture,
not the window; the picture is scaled to the window. It needs the guest patches in `docs/GRAPHICS.md`; if CUSA03762 1.00
does not match them the game stops with a message instead of ignoring the setting.
**Anti-aliasing** (`smaa`: Off, SMAA) and **Texture filtering** (`anisotropy`: Game, 2x, 4x, 8x, 16x).

## Gameplay

**Menu & Subtitles** (`text_language`; restart; English (US) and English (UK) first, then the other languages alphabetically), **Skip Intro** (`skip_intro`, Off by default; restart), **Frame rate** (`frame_rate`: 30, 60, 90, 120, 144, 165, 240, Unlimited),
**Frame rate in title**, **Render resolution in title**, **Frame counter** (`show_fps`; F2 switches it for the session).
The game's logic stays at its own 60 Hz tick; a higher frame rate shows the last frame again between two game flips.
The frame counter is drawn top right at 100%; its corner and size are not launcher options.

## Controls

Controller detection, **Controller Rumble** (`controller_rumble`), keyboard bindings (click a key, press the new one, Esc
cancels, a key may serve more than one control, **Reset to defaults**), the port hotkeys, and the Xbox controller mapping the
runtime applies. A controller SDL does not recognise needs a line in `user\gamecontrollerdb.txt`; `SDL_GAMECONTROLLERCONFIG`
keeps priority and an unmapped joystick is logged once per connection with its GUID.

## Trophies

See `docs/TROPHIES.md`.

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
| `user\gamecontrollerdb.txt` | optional controller mappings |
| `user\savedata\` | save data (created by the runtime when the game writes) |
| `logs\shadow_of_the_beast_NNN.log` | one log per run; the log opens with `Settings:` lines |

Nothing is written to the repository by a player installation. `game\`, `user\`, `logs\` and `build\` are ignored by Git.

## Failure messages

An unsupported package is refused with a diagnostic block written to the log. The block lists the package's
`param.sfo`, the `eboot.bin` hash, the required files that were found or missing, and the supported build. This is
intentional: a different executable can have different code, and the port must not run it with the wrong expectations.
