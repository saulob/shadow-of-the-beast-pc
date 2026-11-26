# Shadow of the Beast PC v@VERSION@

The first public PC release of **Shadow of the Beast** (PS4). The game's original x86-64 code runs natively on Windows x64
and, experimentally, on Linux x86-64 and Steam Deck, through replacement PS4 system libraries and a shadPS4-derived Vulkan
renderer. It is an unofficial fan project, not affiliated with or endorsed by Sony Interactive Entertainment, Heavy
Spectrum Entertainment Labs or any other rights holder.

**No game files are included.** You need your own legally obtained Shadow of the Beast PS4 package (`.pkg`) of a
supported build: Korea `CUSA03762`, USA `CUSA02871` or Europe `CUSA00951`, version **1.00**. The launcher installs it.

## Highlights

- **True Ultrawide Hor+, exclusive to this PC port**: 21:9 and 32:9 show more of the world at the sides while keeping the
  vertical view, without stretching the gameplay image
- **Up to 4K internal render resolution**
- **High frame-rate presentation**: 30, 60 (Original), 90, 120, 144, 165, 240 or Unlimited
- **Native Windows x64** and an experimental **native Linux x86-64 AppImage**
- **Modern graphics options**: SMAA, AMD FSR 1, RCAS sharpening, anisotropic filtering and VSync
- **Xbox and PlayStation controller prompts** in the game's own menus
- **Skip Intro**, **Exit Game** and a configurable **Menu Button Layout**
- **Local PS4 trophies** and save data
- **Built-in Bug Report** with **My reports**, developer replies and optional Windows crash dumps
- **Automatic diagnostics** for unsupported game versions
- **Self-updater**
- **Korea, USA and Europe 1.00** supported, with the same features

## True Ultrawide Hor+

On by default for 21:9 Ultrawide and 32:9 Super Ultrawide, and switchable in the launcher:

- The game world is wider at the sides; the vertical field of view is the original one, so nothing is stretched
- The HUD and menus use the port's safe area: health stays bottom left, the enemy counter bottom right, and the title,
  map, Options and Wisdom of Shadows screens stay centered
- Prerendered videos stay 16:9. The title, map and loading screens only draw their central 16:9 part in the game itself;
  the port fills the sides from the scene

16:10, 4:3 and 5:4 are also offered; they change the shape of the picture without the Hor+ view.

## Up to 4K internal render resolution

Render resolution sets the size the game itself renders at, not a scaling of the window: 480p, 720p, Native (1080p),
1440p, 1800p or 4K. In other shapes the picture keeps the chosen height and follows the shape, within the pixel budget of
4K (32:9 at the 4K setting renders 5408x1520). A start that fails at a high resolution falls back to a safe one.

## High frame rates

Frame rate: 30, 60 (Original, the default), 90, 120, 144, 165, 240 or Unlimited. This is the presentation rate:

- The original game simulation stays at 60 Hz, so gameplay does not speed up
- Above 60, the latest completed game frame is presented again between new game frames, with deadline-based pacing
- There is no motion interpolation: no frames are invented

## Supported game builds

| Region | Title ID | Game version | `eboot.bin` SHA-256 |
| --- | --- | --- | --- |
| Korea | `CUSA03762` | 1.00 | `ebf1d2fa6f44f05ab141e9b06fd8e60179db3be8b0b57b794ce79448ed133023` |
| USA | `CUSA02871` | 1.00 | `ebf1d2fa6f44f05ab141e9b06fd8e60179db3be8b0b57b794ce79448ed133023` |
| Europe | `CUSA00951` | 1.00 | `7c35e4edc6304dc22f665182fb89385a589d04c9d4a7ac54fd8d4652e74a9343` |

All three are base-game packages with the same intended PC-port feature set. Any other build is refused without touching
your installation; when it is still Shadow of the Beast, the launcher writes a read-only diagnosis to the log so support
can be added.

## Also included

- **Display**: Windowed and Borderless Fullscreen (F11), monitor and graphics card selection, Letterbox or Stretch, frame
  counter (F2)
- **Controllers**: Xbox, DualShock 4, DualSense and other SDL gamepads, with controller and keyboard rebinding. Prompts:
  Auto, Xbox or PlayStation
- **Menu Button Layout**: Western (Cross/A confirms) by default, or each region's Original layout
- **Skip Intro** (off by default) skips the publisher and developer logos
- **Exit Game** on the Map and Pause screens, with a confirmation, in every game language
- **Offline**: PlayStation Network prompts and Social Features are turned off
- **Trophies** on the launcher's Trophies page, with your own Trophy Key; **save data** next to the program
- **Launcher**: installs your own `.pkg` after checking the exact build and keeps your settings, saves and logs on
  reinstall. Hold Shift while starting, or start with `--launcher`, to bring it back after turning it off
- **Self-updater**: checks GitHub for a newer release, and downloads and installs it only when you choose to. The game,
  saves, settings and logs are never touched
- **Bug Report**: describe the problem, choose a session log, check the scrubbed preview and press **Send report**.
  Nothing is sent before that and no account is needed. Every report gets a **Report ID**; **My reports** shows its
  status and developer replies. On Windows, a crash dump can be attached

## Windows

`@WINDOWS_ASSET@`: Windows 10 (1903 or newer) or 11, 64-bit, with a Vulkan 1.3 graphics driver. Extract the ZIP into a
folder you can write to, run `shadow_of_the_beast.exe` and choose **Install game...**

The release is not digitally signed. Windows SmartScreen may ask you to confirm the first start (**More info**, **Run
anyway**).

## Linux and Steam Deck (experimental)

`@LINUX_ASSET@`: a native Linux x86-64 program, with no Proton or Wine. It is built on Ubuntu 20.04, so it runs on glibc
2.31 or newer, and needs a Vulkan 1.3 driver. FUSE is not required: without it the AppImage extracts itself for that
run. Put it in a folder of its own, make it executable (`chmod +x`), run it and choose **Install game...**

Linux and Steam Deck are **experimental**: the AppImage is built and checked by the automated tests, but has not yet been
run in gameplay on real Linux or Steam Deck hardware. Reports from Linux and Steam Deck players are very welcome.

It has the same intended features and supports the same three builds as Windows, except:

- Crash dumps are Windows-only; a Linux crash is described in the log, and Bug Report works the same way
- The Direct3D 11 renderer option is Windows-only
- **Install game...** needs `zenity` or `kdialog` for its file dialog
- Holding **Shift** to reopen the launcher works under X11; on Wayland start with `--launcher`

The game, saves, settings and logs live beside the AppImage, or in `~/.local/share/ShadowOfTheBeastPC` when that folder
is read-only.

## Known limitations

- Vulkan 1.3 is required. The OpenGL and Direct3D 11 renderers are experimental and unverified with this game
- Prerendered videos stay 16:9 in other shapes
- Trophies need your own Trophy Key; there is no in-game trophy notification
- 4K and the resolutions above 1080p need a powerful graphics card

## Validation

- The complete headless release gate passed, Windows and Linux
- Supported-build parity passed for `CUSA03762`, `CUSA02871` and `CUSA00951` 1.00, including the build-dependent checks
  against each build's real game image
- The Windows and Linux feature contracts of the release builds are identical
- The Linux release was built and tested on Ubuntu 20.04, the AppImage's glibc 2.31 baseline
- These are automated, headless checks. Gameplay testing has been on Windows

## Security

@VIRUSTOTAL_STATUS@

| File | SHA-256 | VirusTotal |
| --- | --- | --- |
| `@WINDOWS_ASSET@` | `@WINDOWS_SHA256@` | @WINDOWS_VIRUSTOTAL@ |
| `@LINUX_ASSET@` | `@LINUX_SHA256@` | @LINUX_VIRUSTOTAL@ |
| `shadow_of_the_beast.exe` | `@WINDOWS_EXE_SHA256@` | @WINDOWS_EXE_VIRUSTOTAL@ |
| `trg-updater.exe` | `@UPDATER_EXE_SHA256@` | @UPDATER_EXE_VIRUSTOTAL@ |

The `.sha256` files attached to the release hold the ZIP and AppImage hashes as `<sha256>  <file name>`.

## Downloads

- [@WINDOWS_ASSET@](https://github.com/saulob/shadow-of-the-beast-pc/releases/download/v@VERSION@/@WINDOWS_ASSET@)
  ([SHA-256](https://github.com/saulob/shadow-of-the-beast-pc/releases/download/v@VERSION@/@WINDOWS_ASSET@.sha256))
- [@LINUX_ASSET@](https://github.com/saulob/shadow-of-the-beast-pc/releases/download/v@VERSION@/@LINUX_ASSET@)
  ([SHA-256](https://github.com/saulob/shadow-of-the-beast-pc/releases/download/v@VERSION@/@LINUX_ASSET@.sha256))

Full instructions are in the [README](https://github.com/saulob/shadow-of-the-beast-pc#installing).

## Credits

The video core is derived from [shadPS4](https://github.com/shadps4-emu/shadPS4), and the loader, runtime and launcher
grew from the earlier PS4 ports listed in the [README credits](https://github.com/saulob/shadow-of-the-beast-pc#credits).
Thank you to everyone who sends Bug Reports and logs.
