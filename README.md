<div align="center">

# Shadow of the Beast - PC Port

### The PS4 version of Shadow of the Beast running natively on Windows, and experimentally on Linux and Steam Deck, with exclusive True Ultrawide Hor+, up to 4K, 240 fps presentation, modern PC graphics options, and the TRG launcher

[![Latest release](https://img.shields.io/github/v/release/saulob/shadow-of-the-beast-pc?style=for-the-badge&label=release&color=62d5ef&labelColor=10151b&include_prereleases&cacheSeconds=300)](https://github.com/saulob/shadow-of-the-beast-pc/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/saulob/shadow-of-the-beast-pc/total?style=for-the-badge&color=62d5ef&labelColor=10151b&cacheSeconds=300)](https://github.com/saulob/shadow-of-the-beast-pc/releases)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20experimental%20%7C%20Steam%20Deck%20experimental-3d8ea3?style=for-the-badge&labelColor=10151b)

<sub>The game's original PS4 x86-64 code runs directly on the PC, with replacement PS4 system libraries and a Vulkan renderer derived from <a href="https://github.com/shadps4-emu/shadPS4">shadPS4</a>. It is not an emulator. <b>The game itself is not included</b>: bring your own legally obtained Shadow of the Beast PS4 package of a supported build.</sub>

</div>

<img width="1705" height="996" alt="2026-11-26_14-41" src="https://github.com/user-attachments/assets/3f16bb67-d0e0-4c97-afa4-fa482195cdc4" />

## Features

- **True Ultrawide Hor+, exclusive to this PC port**: 21:9 and 32:9 keep the original vertical field of view and show
  more of the world at the sides, without stretching the gameplay image. See [True Ultrawide Hor+](#true-ultrawide-hor)
- **Up to 4K internal render resolution**: 480p, 720p, Native (1080p), 1440p, 1800p or 4K, rendered by the game itself,
  not scaled from the window
- **High frame rates**: 30, 60 (Original, default), 90, 120, 144, 165, 240 or Unlimited frames per second of
  presentation. The game itself still runs at 60 Hz; see [Graphics and display](#graphics-and-display)
- **Graphics**: SMAA anti-aliasing, AMD FSR 1 upscaling, RCAS sharpening, anisotropic filtering and VSync
- **Display**: Windowed and Borderless Fullscreen, monitor and graphics card selection, and the 16:9 Original, 16:10,
  21:9 Ultrawide, 32:9 Super Ultrawide, 4:3 and 5:4 aspect ratios
- **Controllers**: Xbox, DualShock 4, DualSense and other SDL gamepads, with **Xbox or PlayStation button prompts** in the
  game's own menus and a configurable **Menu Button Layout**
- **Skip Intro** and **Exit Game**
- **Local PS4 trophies** and **save data** kept next to the program
- **Launcher with installer**: installs your own `.pkg` after checking that it is a supported build
- **Self-updater**: the launcher can install a newer release; the game, saves, settings and logs are kept
- **Bug Report** in the launcher, with **My reports**, developer replies and an optional Windows crash dump. See
  [Bug Report](#bug-report)
- **Automatic diagnostics** for unsupported game versions, written to the log
- **Native builds**: Windows x64, and an experimental native Linux x86-64 AppImage that needs no Proton or Wine
- **Three supported builds**: Korea, USA and Europe 1.00, with the same features

## What you need

- Windows 10 (1903 or newer) or Windows 11, 64-bit
- **Or, experimental:** Linux x86-64 with glibc 2.31 or newer, including the Steam Deck (see
  [Linux and Steam Deck](#linux-and-steam-deck))
- A graphics card and driver with **Vulkan 1.3**
- A processor with AVX (Intel Sandy Bridge, AMD Bulldozer or newer)
- **Your own legally obtained Shadow of the Beast PS4 package** of one of these builds:

| Region | Title ID | Game version | CONTENT_ID | `eboot.bin` SHA-256 |
| --- | --- | --- | --- | --- |
| Korea | `CUSA03762` | 1.00 | `HP9000-CUSA03762_00-SHADOWOFBEAST0KR` | `ebf1d2fa6f44f05ab141e9b06fd8e60179db3be8b0b57b794ce79448ed133023` |
| USA | `CUSA02871` | 1.00 | `UP9000-CUSA02871_00-SHADOWOFBEAST0US` | `ebf1d2fa6f44f05ab141e9b06fd8e60179db3be8b0b57b794ce79448ed133023` |
| Europe | `CUSA00951` | 1.00 | `EP9000-CUSA00951_00-SHADOWOFBEAST0EU` | `7c35e4edc6304dc22f665182fb89385a589d04c9d4a7ac54fd8d4652e74a9343` |

All three builds have the same intended PC-port feature set. The PC port version (`0.1.0`) identifies the port,
launcher, runtime and renderer; the game version (`1.00`) identifies the original game installed from your own package.
More detail is in [docs/GAME_BUILDS.md](docs/GAME_BUILDS.md).

<img width="1704" height="1006" alt="2026-11-26_14-43" src="https://github.com/user-attachments/assets/205349eb-ddbc-49e6-8cb6-36de8c0a5066" />

<img width="1707" height="1002" alt="2026-11-26_14-43_1" src="https://github.com/user-attachments/assets/36265b7e-1ebd-4578-ad60-4d7b7f29e0ce" />

## Installing

1. Download **ShadowOfTheBeastPC-v&lt;version&gt;-windows-x64.zip** from the
   [latest release](https://github.com/saulob/shadow-of-the-beast-pc/releases/latest).
2. Extract it into a folder you can write to.
3. Run **shadow_of_the_beast.exe**. The launcher opens.
4. On the **Play** page click **Install game...**
5. Select your own `.pkg` of a supported build. It is only read; the game is unpacked into the `game` folder next to the
   program (about 17 GB).
6. Press **PLAY**.

**The game itself is not included**: the release contains no game package, game files or `eboot.bin`, and the port does
not download the game.

To bring the launcher back after turning **Show this launcher** off, hold **Shift** while starting, or start with
`--launcher`.

A package of another version or region is refused, and the installed game is left as it was. When it is still Shadow of
the Beast, the launcher examines it read-only and writes what it found to the log; send that log with
[Bug Report](#bug-report) so the build can be supported.

## True Ultrawide Hor+

True Ultrawide Hor+ is exclusive to this PC port. It is on by default for **21:9 Ultrawide** and **32:9 Super
Ultrawide**, and can be switched off in the launcher.

- **Hor+**: the vertical field of view is the original one, and more of the game world is visible at the sides
- **No stretching**: the gameplay image keeps its proportions
- **HUD in a safe area**: health stays bottom left and the enemy counter bottom right, where you expect them
- **Menus**: the title, map, Options and Wisdom of Shadows screens keep their own centered layouts; on the title and map
  screens the sides are filled from the scene
- **Prerendered videos** stay 16:9

16:10, 4:3 and 5:4 are also offered; they change the shape of the picture without the Hor+ view.

<img width="1918" height="862" alt="2026-11-26_14-46" src="https://github.com/user-attachments/assets/de5f4f58-f5a6-4b9d-97e1-dd206b527ea0" />

## Graphics and display

| Option | Values |
| --- | --- |
| Render resolution | 480p, 720p, Native (1080p), 1440p, 1800p, 4K |
| Aspect ratio | 16:9 Original, 16:10, 21:9 Ultrawide, 32:9 Super Ultrawide, 4:3, 5:4 |
| Frame rate | 30, 60 (Original, default), 90, 120, 144, 165, 240, Unlimited |
| Anti-aliasing | SMAA |
| Upscaling and sharpening | AMD FSR 1, RCAS sharpening with adjustable strength |
| Texture filtering | Anisotropic: Game, 2x, 4x, 8x, 16x |
| Window mode | Windowed, Borderless Fullscreen (**F11** switches while playing) |
| Also | VSync, monitor and graphics card selection, Letterbox or Stretch, frame counter (**F2**) |

**Render resolution** is the size the game itself renders at, not a scaling of the window. Other aspect ratios keep the
chosen height within the pixel budget of 4K. If the game fails to start at a high resolution, it falls back to a safe
one automatically.

**Frame rate** is the presentation rate:

- The original game simulation stays at **60 Hz**, so gameplay does not speed up
- Above 60, the latest completed game frame is presented again between new game frames, with deadline-based pacing
- There is **no motion interpolation**: no frames are invented

More detail is in [docs/GRAPHICS.md](docs/GRAPHICS.md).

## Controllers

- **Xbox**, **DualShock 4**, **DualSense** and other SDL gamepads, plus keyboard
- **Rebinding** for controller and keyboard on the launcher's **Controls** page
- **Controller prompts**: **Auto**, **Xbox** or **PlayStation**. The Xbox button icons appear in the game's own menus
  and prompts. See [docs/CONTROLLER_PROMPTS.md](docs/CONTROLLER_PROMPTS.md)
- **Menu Button Layout**: **Western** (Cross/A confirms, the default) or each region's **Original** layout. See
  [docs/MENU_BUTTONS.md](docs/MENU_BUTTONS.md)

## Saves and trophies

Everything is kept next to the program, so copying the whole folder to another PC takes your data with it:

| Path | Contents |
| --- | --- |
| `user\sotb.ini` | Launcher and game settings |
| `user\savedata\` | Your saves, one folder per region |
| `user\trophy\`, `user\keys.json` | Trophy progress and your Trophy Key |
| `user\bug_reports.json` | The Bug Reports you sent, with their private receipts |
| `logs\` | One log per run (and, on Windows, crash dumps) |
| `game\` | The installed game, unpacked from your own package |

Reinstalling the game or updating the port keeps `user\` and `logs\`.

**Trophies** are local PS4 trophies, kept on this PC and shown on the launcher's **Trophies** page. The trophy list in
your game package is encrypted with a Trophy Key, which the port does not include, generate or download: type your own
key on the **Trophies** page, or import it from an existing shadPS4 `keys.json`. It stays on this PC and is never logged
or sent. Unlocks are recorded even before a key is set and appear once it works. There is no in-game trophy
notification. See [docs/TROPHIES.md](docs/TROPHIES.md).

The game runs offline: the PlayStation Network prompts and Social Features are turned off.

## Bug Report

The launcher's **Bug Report** page sends a report without an account:

1. Describe the problem.
2. Choose the log of that session. Each one says how it ended, for example **Crash detected** or **GPU device lost**.
3. Check the preview. The log is scrubbed before the preview is built: user and computer names, private folders,
   addresses and secrets become placeholders, while crash and graphics diagnostics stay. The preview is exactly what is
   sent.
4. Press **Send report**. Nothing is sent before that.

Every report returns a **Report ID**. **My reports** lists the reports sent from this PC with their status and the
developer's replies. When a fix is ready to try, the report shows **Awaiting player test** and asks **Yes, fixed** or
**No, still happening**; after **No, still happening** you can send a new report with a fresh log.

On Windows, when the game crashed, **Attach crash report** adds that session's crash dump. A crash dump contains part of
the game's process memory (thread stacks and the list of loaded modules); clear the box to send the log alone.

When the installer refuses a game version it does not support, the diagnosis is already in the log; send it the same way.

## Linux and Steam Deck

**Experimental.** This is a native Linux x86-64 build, not Proton or Wine, with the same launcher, intended game
features and supported builds as Windows. The AppImage is built on Ubuntu 20.04, so it runs on glibc 2.31 or newer
(Ubuntu 20.04, SteamOS 3 and later distributions), and needs a Vulkan 1.3 driver. It passes the automated Linux tests and
packaging checks, but **has not yet been tested in gameplay on real Linux or Steam Deck hardware**.

1. Download **ShadowOfTheBeastPC-v&lt;version&gt;-x86_64.AppImage** from the
   [latest release](https://github.com/saulob/shadow-of-the-beast-pc/releases/latest) into a folder of its own.
2. Make it executable (`chmod +x`, or the file manager's **Properties**).
3. Run it. FUSE is not required: without it the AppImage extracts itself and runs from there for that run.
4. Install your own game package from the **Play** page, as on Windows, then press **PLAY**.

On a Steam Deck, do this in Desktop Mode, then add the AppImage to Steam with **Add a Non-Steam Game**. Do not force
Proton: it is a native Linux program. Put `--launcher` in its Launch Options to open the launcher in Gaming Mode.

`game/`, `user/` and `logs/` are created beside the AppImage. When that folder is read-only they go to
`~/.local/share/ShadowOfTheBeastPC`, and the updater cannot install there; `SOTB_PORTABLE_DIR` chooses another folder.

Differences from Windows:

- Crash dumps (`.dmp`) are Windows-only. On Linux a crash still writes its details to the log, and Bug Report works the
  same way.
- The Direct3D 11 renderer option is Windows-only.
- **Install game...** needs `zenity` or `kdialog` for the file dialog.
- Holding **Shift** to reopen the launcher works under X11; on Wayland start with `--launcher`.

More detail is in [docs/LINUX.md](docs/LINUX.md).

## Windows SmartScreen

Windows may warn the first time you run the program, because the release is not digitally signed. If you downloaded it
from this repository's releases page, choose **More info**, then **Run anyway**. The SHA-256 and VirusTotal link of every
release file are in its release notes.

## Reporting problems

The easiest way is the launcher's [Bug Report](#bug-report) page. You can also open an
[issue](https://github.com/saulob/shadow-of-the-beast-pc/issues) and attach the newest log from the `logs` folder.

## Known limitations

- **Linux and Steam Deck** are experimental and not yet tested in gameplay on real hardware.
- **Vulkan 1.3 is required.** The launcher also offers OpenGL and Direct3D 11 renderers, labelled experimental; they have
  not been verified with this game and do not run SMAA, FSR 1, sharpening or the ultrawide side fill.
- **Prerendered videos** stay 16:9 in other aspect ratios.
- **Trophies** need your own Trophy Key, and there is no in-game trophy notification.
- **4K and the other resolutions above 1080p** need a powerful graphics card.

## Building it yourself

Players do not need this: download the release instead.

### Windows (MSYS2 CLANG64)

```powershell
.\tools\setup.ps1                # checks and installs the prerequisites
.\tools\build.ps1                # incremental build into build\cmake\clang64\bin
.\tools\test.ps1                 # headless tests, with the supported-build parity and version contracts
.\tools\test.ps1 -Full           # the complete release gate, Windows and Linux (through WSL)
.\tools\package.ps1              # dist\ShadowOfTheBeastPC-v<version>-windows-x64.zip and its .sha256
.\tools\package-release.ps1      # the whole local release: test gate, Windows ZIP, Linux AppImage, notes
```

`tools\test.ps1` opens no window, starts no game and contacts no server. A supported build without a prepared boot image
is reported as BLOCKED, never as a pass.

`tools\package-release.ps1` runs `tools\test.ps1 -Full`, packages the ZIP, builds and packages the AppImage in an Ubuntu
20.04 WSL distribution, compares the parity contracts of the two release builds, and writes the release notes and
VirusTotal manifests with every entry PENDING. After the four files have been scanned and their URLs recorded,
`tools\package-release.ps1 -NotesOnly` regenerates the notes without building or touching any package (see
[docs/RELEASE_SECURITY.md](docs/RELEASE_SECURITY.md)). Everything stays local: nothing is published, tagged or uploaded.
The release version is the project `VERSION` in `CMakeLists.txt`.

### Linux (Ubuntu 20.04, x86-64)

```sh
sudo bash packaging/linux/ci-deps.sh   # once, on Ubuntu 20.04 (the glibc 2.31 baseline)
bash tools/build.sh --test             # build/cmake/linux/bin/shadow_of_the_beast and the headless Linux suites
bash tools/package-linux.sh            # dist/ShadowOfTheBeastPC-v<version>-x86_64.AppImage and its .sha256
```

See [docs/LINUX.md](docs/LINUX.md) for the requirements and the other commands.

## Repository layout

| Path | Purpose |
| --- | --- |
| `src/` | Native loader, PS4 system-library runtime, game patches, trophies |
| `gpu/` | shadPS4-derived Vulkan video core, shader recompiler, GnmDriver, VideoOut |
| `launcher/` | TRG launcher pages, PKG installer, build identification, Bug Report and updater |
| `server/shadow-bug-reports/` | The Cloudflare Worker that receives Bug Reports, with its dashboard and tests |
| `tests/` | Headless test suites |
| `tools/` | Build, test and local release packaging scripts (Windows and Linux) |
| `scripts/` | Read-only PKG inspection and developer preparation |
| `packaging/` | Application icons, packaged READMEs, release notes template, third-party licenses, Linux build environment |
| `patches/` | The pinned AppImage runtime's patch (the extract-and-run fallback without FUSE) |
| `assets/` | Launcher art and icons, controller prompt sources |
| `docs/` | Launcher, graphics, builds, controls, trophies, Linux, runtime and release documentation |

## Sponsoring

Shadow of the Beast PC is maintained and developed in my free time. If you enjoy the project or find it useful, consider supporting its continued development.

<p align="center">
<a href="https://github.com/sponsors/saulob" target="_blank"><img src="https://img.shields.io/badge/GitHub%20Sponsors-Sponsor-EA4AAA?style=for-the-badge&logo=githubsponsors&logoColor=white" alt="GitHub Sponsors"></a>
</p>

## Credits

- Shadow of the Beast belongs to its respective rights holders. This is an unofficial fan-made PC port. It is not
  affiliated with or endorsed by Sony Interactive Entertainment, Heavy Spectrum Entertainment Labs or any other rights
  holder, and it contains no game package or executable.
- [shadPS4](https://github.com/shadps4-emu/shadPS4) for the Vulkan renderer, shader recompiler, GnmDriver and VideoOut
  the video core is built on (GPL-2.0-or-later; see `gpu/VENDOR.txt`)
- The shared PS4 port infrastructure of the [Dragon's Crown Pro](https://github.com/saulob/dragons-crown-pro-pc),
  [Alienation](https://github.com/saulob/alienation-pc),
  [Odin Sphere Leifthrasir](https://github.com/saulob/odin-sphere-leifthrasir-pc),
  [13 Sentinels: Aegis Rim](https://github.com/saulob/13-sentinels-aegis-rim-pc) and
  [God of War: Sons of Sparta](https://github.com/saulob/god-of-war-sons-of-sparta-pc) PC ports
- [TRG Launcher](https://github.com/TekRantGaming/trg-launcher), the launcher interface (MIT)
- [SMAA](https://github.com/iryoku/smaa) by Jorge Jimenez et al., and AMD FidelityFX Super Resolution 1
- [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) by Alex Barney for ATRAC9 audio decoding
- [FFmpeg](https://ffmpeg.org) (LGPL-2.1-or-later build) for the game's movies
- The Xbox controller outline is from the [Gamepad Line Art Pack](https://gidedin.itch.io/gamepad-line-art-pack) by
  gidedin (CC0)
- The licenses of every library the release includes are in the `licenses` folder of the ZIP, in
  `usr/share/licenses/ShadowOfTheBeastPC` inside the AppImage and in
  [packaging/licenses/THIRD-PARTY-LICENSES.txt](packaging/licenses/THIRD-PARTY-LICENSES.txt)

## AI disclosure

AI tools were used to assist with code analysis, development, debugging and documentation. Changes were directed, tested and reviewed by the maintainer.

## License

Original code in this project is licensed under the GNU General Public License v2.0 or later (GPL-2.0-or-later). See [LICENSE](LICENSE).

Third-party components retain their respective licenses and notices. See [packaging/licenses/THIRD-PARTY-LICENSES.txt](packaging/licenses/THIRD-PARTY-LICENSES.txt).

The game package and executable are not included. Shadow of the Beast and its assets are not covered by this project's license.
