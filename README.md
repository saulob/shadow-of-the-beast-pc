<div align="center">

# Shadow of the Beast - PC Port

### Shadow of the Beast (PS4), running natively on Windows through a shadPS4-derived runtime and the TRG Launcher

**Status: bring-up foundation.** The launcher and native runtime load and validate the game, but gameplay has not been
tested yet. No game files are included.

</div>

## What this is

The original PS4 x86-64 executable runs on the PC. The port supplies the PS4 system libraries, a Vulkan renderer derived
from [shadPS4](https://github.com/shadps4-emu/shadPS4), input, audio and filesystem services. It is not an emulator.

This first round provides:

- **TRG Launcher** with **Install game...** from your own `.pkg`
- exact build identification before anything is unpacked
- installation into `game\prepared\CUSA03762-1.00`, with `user\` and `logs\` kept separate
- native boot image preparation and import resolution
- the shadPS4-derived GNM/VideoOut foundation
- a runtime log that names the first unsupported PS4 import instead of failing silently

Gameplay, movies, audio output and controller support are not declared working yet. See [Runtime status](#runtime-status).

## Supported build

| | |
| --- | --- |
| Game | Shadow of the Beast |
| Platform | PlayStation 4 |
| Title ID | `CUSA03762` |
| Content ID | `HP9000-CUSA03762_00-SHADOWOFBEAST0KR` |
| Region | Korea |
| Category | `gd` (base game) |
| APP_VER | `01.00` |
| VERSION | `01.01` |
| System version | `0x04500000` (firmware 4.50) |
| Engine | Unreal Engine 4.8 (build `++depot+UE4-Releases+4.8`, EngineVersion `2591939`) |
| `eboot.bin` size | 75,181,963 bytes |
| `eboot.bin` SHA-256 | `ebf1d2fa6f44f05ab141e9b06fd8e60179db3be8b0b57b794ce79448ed133023` |

The package is identified by `param.sfo`, not by its file name. The launcher checks TITLE_ID, CONTENT_ID, CATEGORY,
APP_VER and VERSION before unpacking, then the executable hash before activating the installation. Any other build is
refused with a diagnostic block in the log.

The package must be a decrypted (fake) PS4 PKG. A package encrypted for a retail console cannot be unpacked on a PC.

## Using it

1. From the repository, run `.\tools\build.ps1 -Run`. It builds and opens the launcher.
2. On **Play**, click **Install game...** and select your own Shadow of the Beast `.pkg`.
3. Wait for **Ready to play**. The package is only read; the game is unpacked into `game\` next to the program (about
   17 GB).
4. Press **PLAY**. The runtime log is written to `build\logs\shadow_of_the_beast_NNN.log` when started this way.

Processor: an x86-64 CPU with SSE4.1, SSE4.2, POPCNT and AVX that the operating system saves (Intel Sandy Bridge, AMD
Bulldozer or newer). MOVBE, BMI1 and LZCNT are handled in software when the processor lacks them. The launcher
refuses to start the game when a required set is missing and says which.

`user\sotb.ini` holds launcher settings. `user\savedata\` is reserved for saves. The installed game is immutable from
the launcher's point of view; reinstalling replaces it and keeps `user\` and `logs\`.

## Building

Requirements: Windows 10 1903+ or Windows 11 x64, MSYS2, and the CLANG64 packages installed by `tools\setup.ps1`.

```powershell
.\tools\setup.ps1
.\tools\build.ps1 -Run
```

`tools\build.ps1` fetches the pinned video-core dependencies and TRG Launcher, configures CMake + Ninja, builds
`shadow_of_the_beast.exe`, copies every DLL it loads beside it, and with `-Run` starts the launcher and stays attached
until it closes. Without `-Run` it only builds. Extra arguments after `-Run` are forwarded to the loader.

The launcher builds its boot image on install, so `tools\prepare-game.ps1` is only a developer convenience.

## Runtime status

This round deliberately stops at the first unhandled import. The log shows:

- the detected build fingerprint and executable hash
- linked modules and their initializers
- every import resolved natively
- the first unsupported PS4 import with its NID, library, symbol name and caller offset
- fatal exception address, registers, stack words and module context
- the last initialization stage before a crash

Known gaps for the next round:

- **libSceAvPlayer** is not implemented yet. The game has 30 MP4 movies and imports 20 AvPlayer functions.
- **libSceNgs2** is not implemented.
- **libSceAudio3d** is implemented as a stereo renderer over AudioOut; it has not been heard yet.
- **libSceAjm** decodes ATRAC9 through LibAtrac9; other codecs stop explicitly.
- **Online / PSN** services report signed-out or unavailable; they are not a goal of this port.
- Trophies are not implemented.
- The in-game Options menu is not exposed yet; only launcher settings that exist are shown.
- Texture/shader translation for UE4.8 has not been exercised by a running frame.

## Repository layout

```text
assets/launcher/      header art and launcher assets
docs/                 launcher, build and runtime notes
gpu/                  shadPS4-derived video core (GNM, VideoOut, shader recompiler, Vulkan)
launcher/             TRG Launcher, PKG installer, build fingerprint
packaging/            icon and third-party notices
scripts/              read-only PKG inspection and developer preparation
src/                  native loader and PS4 system-library runtime
tools/                build.ps1 and setup.ps1
user/                 settings and saves (created at run time, not tracked)
logs/                 run logs (created at run time, not tracked)
game/                 installed game (created by the launcher, not tracked)
```

## Credits

The runtime and video core are based on [shadPS4](https://github.com/shadps4-emu/shadPS4) (GPL-2.0-or-later) and the
shared PS4 port infrastructure from Saulo's [Dragon's Crown Pro](https://github.com/saulob/dragons-crown-pro-pc),
[Alienation](https://github.com/saulob/alienation-pc),
[Odin Sphere Leifthrasir](https://github.com/saulob/odin-sphere-leifthrasir-pc) and
[13 Sentinels: Aegis Rim](https://github.com/saulob/13-sentinels-aegis-rim-pc) projects. The launcher is
[TRG Launcher](https://github.com/TekRantGaming/trg-launcher). ATRAC9 decoding is [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) (MIT). Movies are decoded by [FFmpeg](https://ffmpeg.org) (LGPL-2.1-or-later build, game codecs only).

**No game files are included.** You need your own legally obtained copy of the game.
