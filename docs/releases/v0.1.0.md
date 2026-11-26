# Shadow of the Beast PC v0.1.0

The first public release of **Shadow of the Beast** (PS4) on PC. The game's original x86-64 code runs on your PC
through replacement PS4 system libraries and a shadPS4-derived Vulkan renderer: the game you know, with PC options.

**No game files are included.** You need your own legally obtained Shadow of the Beast PS4 package (`.pkg`) of a
supported build; the launcher installs it.

## Highlights

- **True Ultrawide Hor+** for 21:9 and 32:9, with the HUD and menus kept in their 16:9 layout
- **Render resolutions up to 4K**, and frame rates from 30 to 240 or Unlimited
- **Xbox or PlayStation button prompts** in the game's own menus, and a Western or Original menu button layout
- **Skip Intro** and **Exit Game**
- **Three regional builds**: Korea, USA and Europe, with the same features
- **Bug Report** in the launcher, with developer replies and player tests

## Supported game builds

| Region | Title ID | Game version | `eboot.bin` SHA-256 |
| --- | --- | --- | --- |
| Korea | `CUSA03762` | 1.00 | `ebf1d2fa6f44f05ab141e9b06fd8e60179db3be8b0b57b794ce79448ed133023` |
| USA | `CUSA02871` | 1.00 | `ebf1d2fa6f44f05ab141e9b06fd8e60179db3be8b0b57b794ce79448ed133023` |
| Europe | `CUSA00951` | 1.00 | `7c35e4edc6304dc22f665182fb89385a589d04c9d4a7ac54fd8d4652e74a9343` |

All three are base-game packages and get the same features. Any other build is refused without touching your
installation; when it is still Shadow of the Beast, the launcher writes a read-only diagnosis to the log so support can
be added.

## Display and graphics

- Render resolution: 480p, 720p, Native (1080p), 1440p, 1800p, 4K. A start that fails at a high resolution falls back
  to a safe one
- Aspect ratio: 16:9 Original, 16:10, 21:9 Ultrawide, 32:9 Super Ultrawide, 4:3, 5:4
- True Ultrawide (on by default for 21:9 and 32:9): a wider view in gameplay and on the title, health bottom left, enemy
  counter bottom right, map, Options and Wisdom of Shadows centered, filled sides on the title, map and loading screens
- Frame rate: 30, 60 (default), 90, 120, 144, 165, 240 or Unlimited. The game's logic keeps its own 60 Hz tick; higher
  rates repeat the latest frame and do not speed the game up
- Vulkan 1.3 renderer with SMAA, AMD FSR 1, RCAS sharpening, anisotropic filtering, VSync and graphics card selection
- Windowed and Borderless Fullscreen (F11), monitor selection, Letterbox or Stretch, frame counter (F2)

## Controls and game integration

- Xbox, DualShock 4, DualSense and other SDL controllers, with controller and keyboard rebinding
- Controller prompts: Auto, Xbox or PlayStation
- Menu Button Layout: Western (Cross/A confirms) by default, or each region's Original layout
- Skip Intro (off by default) skips the publisher and developer logos
- Exit Game on the Map and Pause screens, with a confirmation, in every game language
- Offline: PlayStation Network prompts and Social Features are turned off
- Local trophies on the launcher's Trophies page, with your own Trophy Key
- Save data next to the program

## Launcher

- Installs your own `.pkg` after checking the exact build, and keeps your settings, saves and logs on reinstall
- Eight pages: Play, Display, Graphics, Gameplay, Controls, Trophies, Bug Report and About
- Hold Shift while starting, or start with `--launcher`, to bring the launcher back after turning it off
- Self-updater: checks GitHub for a newer release, and downloads and installs it only when you choose to. The game,
  saves, settings and logs are never touched

## Bug reporting and crash diagnostics

- Describe the problem, choose a session log, check the scrubbed preview and press **Send report**. Nothing is sent
  before that, and no account is needed
- Every report gets a **Report ID**. **My reports** shows its status and developer replies, and asks **Yes, fixed** or
  **No, still happening** when a fix is ready to try
- On Windows, a crash writes a small crash dump (thread stacks and module list) that you can attach to the report; an
  interrupted upload resumes under the same Report ID

## Linux and Steam Deck

A native Linux x86-64 **AppImage** (no Proton or Wine), built on Ubuntu 20.04 so it runs on glibc 2.31 or newer. It is
**experimental**: it is built and checked by the automated tests, but has not been run on real Linux or Steam Deck
hardware yet. FUSE is not required; without it the AppImage extracts itself for that run.

It has the same features and supports the same three builds as Windows, except:

- Crash dumps are Windows-only; a Linux crash is described in the log, and Bug Report works the same way
- The Direct3D 11 renderer option is Windows-only
- **Install game...** needs `zenity` or `kdialog` for its file dialog
- Holding **Shift** to reopen the launcher works under X11; on Wayland start with `--launcher`

The game, saves, settings and logs live beside the AppImage, or in `~/.local/share/ShadowOfTheBeastPC` when that folder
is read-only. The launcher's updater replaces the AppImage file itself.

## Known limitations

- Vulkan 1.3 is required. The OpenGL and Direct3D 11 renderers are experimental and unverified with this game
- Prerendered videos stay 16:9 in other shapes
- Trophies need your own Trophy Key; there is no in-game trophy notification
- 4K and the resolutions above 1080p need a powerful graphics card

## Installation

- **Windows**: download `ShadowOfTheBeastPC-v0.1.0-windows-x64.zip`, extract it, run `shadow_of_the_beast.exe` and choose
  **Install game...**
- **Linux**: download `ShadowOfTheBeastPC-v0.1.0-x86_64.AppImage` into a folder of its own, make it executable
  (`chmod +x`), run it and choose **Install game...**

Full instructions are in the [README](https://github.com/saulob/shadow-of-the-beast-pc#installing).

## Checksums

| File | SHA-256 |
| --- | --- |
| `ShadowOfTheBeastPC-v0.1.0-windows-x64.zip` | `@WINDOWS_SHA256@` |
| `ShadowOfTheBeastPC-v0.1.0-x86_64.AppImage` | `@LINUX_SHA256@` |

The release is not digitally signed. Windows SmartScreen may ask you to confirm the first start (**More info**, **Run
anyway**).
