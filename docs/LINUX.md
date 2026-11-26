# Linux

Linux x86_64 builds from the same sources as Windows (`tools/build.sh`: Clang, CMake and Ninja) and ships as an
AppImage, `ShadowOfTheBeastPC-v<version>-x86_64.AppImage`. It is experimental: it is built and checked by the headless
tests, but has not been run on real Linux or Steam Deck hardware.

## The release baseline

Release AppImages are built on **Ubuntu 20.04**, whose glibc (**2.31**) is the oldest a player's system may have.
`tools/package-linux.sh` refuses to package on a newer glibc, and reads the GLIBC versions every shipped ELF needs (the
program, the bundled libraries, the fallback C++ libraries and the AppImage runtime): any newer than 2.31 stops it.

`packaging/linux/ci-deps.sh` prepares a clean 20.04 system once, as root: Clang 20, CMake 3.31.8, NASM 2.16.03 and make
for FFmpeg, patchelf, binutils and desktop-file-utils for packaging, the distribution's development packages, and
pinned, checksum-verified builds of what 20.04 lacks (Vulkan-Headers, VMA, glslang, Boost headers, fmt, Zydis, xxHash,
robin-map, OpenSSL, SDL3). In WSL2, from the repository folder:

```powershell
wsl --import SotbFocalBuild <folder> ubuntu-base-20.04.5-base-amd64.tar.gz --version 2
wsl -d SotbFocalBuild -u root -- bash packaging/linux/ci-deps.sh
```

Any Ubuntu 20.04 distribution prepared the same way works; `tools\package-release.ps1` finds one by its glibc. A newer
distribution (for example Ubuntu 24.04) is fine for development and for `tools\test.ps1 -Full`, but what it builds needs
a newer glibc and is never packaged. No script installs anything by itself: `tools/build.sh`, `tools/test-linux.sh` and
`tools\test.ps1` list what is missing and stop with exit code 3 (BLOCKED).

## FFmpeg

`tools/build-ffmpeg-linux.sh` builds the LGPL FFmpeg pinned in `gpu/ffmpeg.txt` into `cache/deps/ffmpeg/<version>/linux`.
Building it needs NASM, make, curl, tar and xz; a prefix whose stamp matches the pin and the script is used as it is,
without them. The program finds it through its build RPATH; the AppImage carries those libraries in `usr/lib`.

## Commands

| Command | Result |
| --- | --- |
| `tools/build.sh` | fetches `gpu/deps.txt` and FFmpeg, builds `build/cmake/linux/bin/shadow_of_the_beast` |
| `tools/build.sh --test` | also builds and runs the headless suites (`tools/test-linux.sh`) |
| `tools/test-linux.sh --parity` | builds and runs only `sotb-build-parity-tests` |
| `tools/package-linux.sh` | builds, then writes `dist/ShadowOfTheBeastPC-v<version>-x86_64.AppImage` and its `.sha256` |
| `tools/package-linux.sh --verify-appimage FILE` | extracts an AppImage (never starting it) and checks everything in it |
| `tools/package-linux.sh --extract-program FILE OUT` | copies `usr/bin/shadow_of_the_beast` out of an AppImage with `unsquashfs`, never running it |
| `tools/package-release-linux.sh` | from a checkout on a Windows drive: mirrors it, builds, tests, runs the parity contract, packages, copies to `dist/` |
| `tools\package-release.ps1` | Windows: the complete release, `test.ps1 -Full` to VirusTotal manifests ([RELEASE_SECURITY.md](RELEASE_SECURITY.md)) |
| `tools/build.sh --run` | starts the program that was just built |

`tools/package-release-linux.sh` mirrors the sources into `~/.cache/shadow-of-the-beast-pc-build` inside WSL (game data,
user data, builds and `cache/` never cross) and builds there. The only exception is `build/image/<build>/boot.bin` and
`image.json` of the supported builds, which the parity contract reads; the packaging refuses both names in an AppImage.

## The AppImage

- `usr/bin/shadow_of_the_beast` without debug information (the symbols go to `dist/<name>-symbols`), with DT_RPATH
  `$ORIGIN/../lib`; a DT_RUNPATH or a build path in it stops the packaging
- `usr/lib`: the libraries it loads that a player's system may lack (SDL3, the pinned FFmpeg, FreeType, libpng, fmt,
  Zydis, xxHash, OpenSSL's libcrypto and what they need), each with its licence text and the package or source it came
  from in `usr/share/licenses/ShadowOfTheBeastPC/SYSTEM-PACKAGES.txt`
- `usr/lib/compat`: libstdc++ and libgcc_s, used by `AppRun` only when the system's copies are older than the build's
- Never bundled: glibc, the Vulkan loader and GPU drivers, GL/EGL, X11, Wayland, audio and D-Bus. The player's
  system provides them, so its own drivers (Mesa on a Steam Deck, NVIDIA) are always used
- The launcher images, the desktop entry, the AppStream metadata, the icon, `README.txt` and the licences

Before writing the AppImage, and again on its extracted contents, the packaging refuses game data, PS4 packages, user
data, logs, dumps, Bug Report receipts, test programs and fixtures, object and debug files, RenderDoc and Vulkan layers,
Wrangler state, tokens, and local paths in ASCII or UTF-16 text.

### Runtime

The runtime at the front of the AppImage is upstream [type2-runtime](https://github.com/AppImage/type2-runtime) at the
commit pinned in `patches/appimage-runtime/baseline.env`, with one patch: when FUSE cannot mount the image, it extracts
it to a fresh temporary folder for that run and removes it afterwards, so a system without FUSE needs no setup.
`tools/build-appimage-runtime.sh` builds it (as root, in an Alpine chroot) or takes an existing copy, and accepts it
only with the pinned SHA-256. appimagetool 1.9.1 is pinned the same way. The packaging checks that the runtime embedded
in the written AppImage is byte for byte that file, apart from the sections appimagetool fills.

### Data and updates

The AppImage's mount is read-only. `game/`, `user/` and `logs/` live beside the AppImage file; when that folder is
read-only, in `$XDG_DATA_HOME/ShadowOfTheBeastPC` (normally `~/.local/share/ShadowOfTheBeastPC`). `SOTB_PORTABLE_DIR`
chooses another folder.

The updater downloads `ShadowOfTheBeastPC-v<version>-x86_64.AppImage` and its `.sha256`, checks the SHA-256, writes the
new file beside the installed one, makes it executable and renames it over the original (`$APPIMAGE`, never the mount or
an extraction folder), then starts that file again. If anything fails, the original AppImage stays. Updates are offered
only when the AppImage's folder is writable.

## Differences from Windows

- Crash dumps (`.dmp`) are Windows-only; a Linux crash writes its details to the log
- The Direct3D 11 renderer is Windows-only
- **Install game...** needs `zenity` or `kdialog` for its file dialog
- Holding Shift at start reopens the launcher only under X11; `--launcher` (or the desktop entry's **Show launcher**)
  works everywhere
