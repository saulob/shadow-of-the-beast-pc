# Linux

Linux x86_64 builds from the same sources as Windows (`tools/build.sh`, Clang, CMake and Ninja). It has not been built
or run yet.

## Preparing a machine

Release builds use Ubuntu 20.04: its glibc (2.31) is the oldest a release may need. `packaging/linux/ci-deps.sh` prepares
a clean 20.04 system once, as root: Clang 20, CMake 3.31.8, nasm and make for FFmpeg, the distribution's development
packages, and pinned, checksum-verified builds of what 20.04 lacks (Vulkan-Headers, VMA, glslang, Boost headers, fmt,
Zydis, xxHash, robin-map, OpenSSL, SDL3). In WSL2, from the repository folder:

```powershell
wsl --import SotbFocalBuild <folder> ubuntu-base-20.04.5-base-amd64.tar.gz --version 2
wsl -d SotbFocalBuild -u root -- bash packaging/linux/ci-deps.sh
```

A newer distribution (for example Ubuntu 24.04 in WSL2) works for development and for `tools\test.ps1 -Full`; what it
builds is not a release. Install the tools and libraries `tools/build.sh` lists from its packages.

No script installs anything by itself: `tools/build.sh`, `tools/test-linux.sh` and `tools\test.ps1` list what is
missing and stop with exit code 3 (BLOCKED).

## FFmpeg

`tools/build-ffmpeg-linux.sh` builds the FFmpeg pinned in `gpu/ffmpeg.txt` into `cache/deps/ffmpeg/<version>/linux`.
Building it needs nasm (FFmpeg's x86 assembly is kept), make, curl, tar and xz. A prefix whose stamp matches the
current pin and script, and which holds every library, header and licence file, is used as it is; then none of those
tools is needed. `tools/build-ffmpeg-linux.sh --check PREFIX WORK` reports which case applies without building.

## Commands

| Command | Result |
| --- | --- |
| `tools/build.sh` | fetches `gpu/deps.txt` and FFmpeg, builds `build/cmake/linux/bin/shadow_of_the_beast` |
| `tools/build.sh --test` | also builds and runs the headless tests (`tools/test-linux.sh`) |
| `tools/test-linux.sh --parity` | builds and runs only `sotb-build-parity-tests` |
| `tools/build.sh --run` | starts the program that was just built |
