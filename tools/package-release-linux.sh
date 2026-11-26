#!/usr/bin/env bash
# The Linux release flow for a checkout on a Windows drive (tools\package-release.ps1 starts it through wsl.exe in the
# Ubuntu 20.04 distribution): mirrors the sources into a Linux folder, builds, runs the headless Linux suites and the
# parity contract (into the checkout's build/release/), packages and verifies the AppImage, and copies it into the
# checkout's dist/. Local only: nothing is published or uploaded, and the program is never started.
#
#   tools/package-release-linux.sh [--clean] [--skip-build]
#
# A Windows drive is slow to build on and has no Unix file modes, so only what changed is mirrored into
# ${SOTB_WSL_BUILD_DIR:-~/.cache/shadow-of-the-beast-pc-build}. Needs packaging/linux/ci-deps.sh's environment and rsync.
set -euo pipefail
source=$(cd -- "$(dirname -- "$0")/.." && pwd)
clean=0 skip_build=0
while (($#)); do
    case $1 in
        --clean) clean=1 ;;
        --skip-build) skip_build=1 ;;
        -h|--help) sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

step() { printf '\n==> %s\n' "$*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || die 'This needs Linux x86_64 (WSL2 on a 64-bit PC).'
command -v rsync >/dev/null || die 'rsync is missing (packaging/linux/ci-deps.sh installs it).'

mirror=${SOTB_WSL_BUILD_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/shadow-of-the-beast-pc-build}
mkdir -p "$mirror"
mirror=$(cd -- "$mirror" && pwd)
[[ $mirror != "$source" && $mirror != "$source"/* ]] || die "The build folder $mirror is inside the checkout."

step "Mirroring the sources into $mirror"
# Content, not timestamps, decides what is copied: a restored older file must still rebuild. Game data, user data,
# builds (including the Windows FFmpeg and any FFmpeg built on a newer system) never cross.
rsync -rl --checksum --no-times --delete --chmod=D755,F644 \
    --exclude '/.git' --exclude '/game' --exclude '/user' --exclude '/cache' --exclude '/logs' --exclude '/build' \
    --exclude '/dist' --exclude '/out' --exclude '/.venv' --exclude 'node_modules' --exclude '.wrangler' \
    "$source/" "$mirror/"
chmod +x "$mirror"/tools/*.sh "$mirror"/packaging/linux/*.sh "$mirror"/packaging/linux/AppRun
# The parity contract reads each supported build's prepared boot image; nothing else from build/ crosses.
[[ -d $source/build/image ]] || die 'No prepared boot images in build/image (install the supported builds with the launcher).'
mkdir -p "$mirror/build/image"
rsync -rt --delete --chmod=D755,F644 --include '/*/' --include '/*/boot.bin' --include '/*/image.json' --exclude '*' \
    "$source/build/image/" "$mirror/build/image/"

release=$source/build/release
mkdir -p "$release"
rm -f -- "$release/feature-contract-linux.txt" "$release/linux-program.sha256"

cd "$mirror"
((clean)) && rm -rf build/cmake/linux build/appimage
rm -rf dist
if ((!skip_build)); then
    step 'Linux build'
    bash tools/build.sh
    step 'Headless Linux suites (nothing is started)'
    bash tools/test-linux.sh --full
fi
step 'Parity contract of this build (nothing is started)'
code=0
bash tools/test-linux.sh --parity > "$release/feature-contract-linux.txt" 2>&1 || code=$?
echo "EXIT|$code" >> "$release/feature-contract-linux.txt"
grep -E '^(GROUP|VERSION)\|' "$release/feature-contract-linux.txt" || true
bash tools/package-linux.sh --skip-build
sha256sum -- build/appimage/AppDir/usr/bin/shadow_of_the_beast | cut -d' ' -f1 > "$release/linux-program.sha256"

step "Copying the AppImage into $source/dist"
shopt -s nullglob
images=(dist/ShadowOfTheBeastPC-v*-x86_64.AppImage)
((${#images[@]} == 1)) || die "Expected one AppImage in $mirror/dist, found ${#images[@]}"
name=$(basename -- "${images[0]}" .AppImage)
mkdir -p "$source/dist"
rm -rf -- "$source/dist/$name.AppImage" "$source/dist/$name.AppImage.sha256" "$source/dist/$name-symbols"
cp -- "dist/$name.AppImage" "dist/$name.AppImage.sha256" "$source/dist/"
cp -r -- "dist/$name-symbols" "$source/dist/"
cmp -s -- "dist/$name.AppImage" "$source/dist/$name.AppImage" || die 'the copied AppImage differs from the verified one'
(cd "$source/dist" && sha256sum -c -- "$name.AppImage.sha256")
printf '\nLinux AppImage: %s\n' "$source/dist/$name.AppImage"
