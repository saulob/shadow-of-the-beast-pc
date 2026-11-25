#!/usr/bin/env bash
# Builds shadow_of_the_beast on Linux x86_64 incrementally (Clang, CMake + Ninja): the Linux counterpart of tools\build.ps1.
#
#   tools/build.sh                   build build/cmake/linux/bin/shadow_of_the_beast
#   tools/build.sh --clean           delete build/cmake/linux first
#   tools/build.sh --configure       only prepare the dependencies and the build folder
#   tools/build.sh --test            also build and run the headless tests (tools/test-linux.sh)
#   tools/build.sh --run [args...]   start the program that was just built (arguments after --run go to it)
#
# Exit code 3: a tool or library is missing (listed, with the way to prepare the machine); the third-party sources and
# FFmpeg are fetched by pinned hash, and FFmpeg's own build tools are needed only when it must be built.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$root/build/cmake/linux
config=RelWithDebInfo
clean=0 configure_only=0 test=0 run=0
loader_args=()
while (($#)); do
    case $1 in
        --clean) clean=1 ;;
        --configure) configure_only=1 ;;
        --test) test=1 ;;
        --run) run=1; shift; loader_args=("$@"); break ;;
        -h|--help) sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

step() { printf '\n==> %s\n' "$*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || die 'This script builds for Linux x86_64 only (Windows: tools\build.ps1).'

export CC=${CC:-clang} CXX=${CXX:-clang++}
missing=()
for tool in "$CC" "$CXX" cmake ninja pkg-config git; do
    command -v "$tool" >/dev/null || missing+=("$tool")
done
command -v glslang >/dev/null || command -v glslangValidator >/dev/null || missing+=(glslang)
if command -v pkg-config >/dev/null; then
    for module in sdl3 freetype2 libpng zlib openssl libxxhash vulkan; do
        pkg-config --exists "$module" || missing+=("pkg-config:$module")
    done
fi
if ((${#missing[@]})); then
    echo "Missing: ${missing[*]}"
    echo 'Prepare: sudo bash packaging/linux/ci-deps.sh on the Ubuntu 20.04 release environment (docs/LINUX.md); on a development distribution, install the listed tools and libraries from its packages'
    exit 3
fi

step "Toolchain $($CC --version | head -n 1), $config"

step 'Third-party sources (gpu/deps.txt)'
patches_of() {
    local LC_ALL=C dir=$1 file
    PATCHES=()
    for file in "$dir"/*.patch; do
        [[ -f $file ]] || continue
        PATCHES+=("$file")
    done
}
# The same fingerprint as Get-PatchFingerprint in tools\_common.ps1, so a checkout fetched on Windows stays current.
patches_fingerprint() {
    local file hash
    (($#)) || return 0
    for file in "$@"; do
        hash=$(sha256sum <"$file" | cut -d' ' -f1)
        printf '%s:%s\n' "$(basename "$file")" "${hash^^}"
    done | sha256sum | cut -d' ' -f1 | tr 'a-f' 'A-F'
}
while read -r path url commit; do
    [[ -z ${path:-} || $path == \#* ]] && continue
    dir=$root/gpu/$path
    patches_of "$root/gpu/patches/$(basename "$path")"
    fingerprint=$(patches_fingerprint "${PATCHES[@]}")
    expected=$commit
    [[ -n $fingerprint ]] && expected=$commit$'\n'$fingerprint
    [[ -f $dir/.sotb_commit && $(tr -d '\r' <"$dir/.sotb_commit") == "$expected" ]] && continue
    echo "Fetching $path (${commit:0:7})"
    rm -rf -- "$dir"
    mkdir -p -- "$dir"
    git -C "$dir" init --quiet
    git -C "$dir" fetch --quiet --depth 1 "$url" "$commit"
    git -C "$dir" -c advice.detachedHead=false checkout --quiet FETCH_HEAD
    [[ -f $dir/.gitmodules ]] && git -C "$dir" submodule update --quiet --init --recursive --depth 1
    for patch in "${PATCHES[@]}"; do
        git -C "$dir" apply "$patch"
    done
    printf '%s' "$expected" >"$dir/.sotb_commit"
done < <(tr -d '\r' <"$root/gpu/deps.txt")

step 'FFmpeg (gpu/ffmpeg.txt)'
ffmpeg_version=$(tr -d '\r' <"$root/gpu/ffmpeg.txt" | awk '$1 == "version" { print $2 }')
ffmpeg=$root/cache/deps/ffmpeg/$ffmpeg_version/linux
bash "$root/tools/build-ffmpeg-linux.sh" "$ffmpeg" "$root/cache/deps/ffmpeg/$ffmpeg_version/work-linux" || exit $?

((clean)) && { step 'Cleaning build/cmake/linux'; rm -rf -- "$build_dir"; }
cache=$build_dir/CMakeCache.txt
if [[ ! -f $build_dir/build.ninja ]] || ! grep -qs "CMAKE_BUILD_TYPE:STRING=$config" "$cache" ||
   ! grep -qsF "SOTB_FFMPEG_PREFIX:PATH=$ffmpeg" "$cache"; then
    step 'Configuring'
    fresh=()
    [[ -f $cache ]] && fresh=(--fresh)
    cmake -S "$root" -B "$build_dir" -G Ninja "${fresh[@]}" -DCMAKE_BUILD_TYPE=$config \
        -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DSOTB_FFMPEG_PREFIX="$ffmpeg"
fi
((configure_only)) && exit 0

step 'Building'
ninja -C "$build_dir" shadow_of_the_beast
loader=$build_dir/bin/shadow_of_the_beast
[[ -x $loader ]] || die "Missing $loader"
printf '\nBuilt %s\n' "$loader"

if ((test)); then
    step 'Headless tests'
    bash "$root/tools/test-linux.sh"
fi
((run)) || exit 0
cd -- "$(dirname -- "$loader")"
exec "$loader" "${loader_args[@]}"
