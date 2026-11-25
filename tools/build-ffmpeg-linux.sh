#!/usr/bin/env bash
# Builds the FFmpeg pinned in gpu/ffmpeg.txt into PREFIX on Linux (LGPL, shared); tools/build.sh runs it. Nothing is rebuilt
# while PREFIX holds the build of the current pins, and nothing is installed outside PREFIX and WORK.
#
#   tools/build-ffmpeg-linux.sh PREFIX WORK            build when PREFIX is not current
#   tools/build-ffmpeg-linux.sh --check PREFIX WORK    only report FFMPEG|current, FFMPEG|build or FFMPEG|blocked
#
# Exit 3: a build is needed and a tool it needs is missing (listed, with the way to prepare the machine).
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
die() { echo "ERROR: $*" >&2; exit 1; }
check_only=0
if [[ ${1:-} == --check ]]; then
    check_only=1
    shift
fi
prefix=$(realpath -m -- "${1:?usage: tools/build-ffmpeg-linux.sh [--check] PREFIX WORK}")
work=$(realpath -m -- "${2:?usage: tools/build-ffmpeg-linux.sh [--check] PREFIX WORK}")

pin=$root/gpu/ffmpeg.txt
field() { tr -d '\r' <"$pin" | awk -v key="$1" '$1 == key { $1 = ""; sub(/^ /, ""); print }'; }
version=$(field version) url=$(field url) sha256=$(field sha256)
[[ -n $version && $url == https://* && $sha256 =~ ^[0-9a-f]{64}$ ]] || die "$pin needs version, an https url and sha256"
read -r -a options <<<"$(field configure) --enable-pthreads --enable-pic"
cc=${CC:-clang}
licenses=$prefix/share/licenses/ffmpeg
stamp=$licenses/sotb-build.stamp
tarball=$work/ffmpeg-$version.tar.xz
names=(avcodec avformat avutil swscale swresample)
expected=$(cat "$pin" "${BASH_SOURCE[0]}" | sha256sum | cut -d' ' -f1)

prefix_problem() {
    local dir=$1 name
    for name in "${names[@]}"; do
        compgen -G "$dir/lib/lib$name.so.*" >/dev/null || { echo "lib/lib$name.so is missing"; return; }
        [[ -f $dir/lib/pkgconfig/lib$name.pc ]] || { echo "lib$name.pc is missing"; return; }
        [[ -f $dir/include/lib$name/$name.h ]] || { echo "include/lib$name/$name.h is missing"; return; }
    done
    for name in BUILD.txt LICENSE.md COPYING.LGPLv2.1; do
        [[ -f $dir/share/licenses/ffmpeg/$name ]] || { echo "share/licenses/ffmpeg/$name is missing"; return; }
    done
    ! grep -qE -- '--enable-(gpl|nonfree)' "$dir/share/licenses/ffmpeg/BUILD.txt" || echo 'BUILD.txt names a non-LGPL configuration'
}

apt_package() {
    case $1 in
        xz) echo xz-utils ;;
        sha256sum) echo coreutils ;;
        awk) echo gawk ;;
        *) echo "$1" ;;
    esac
}

if [[ -f $stamp && $(<"$stamp") == "$expected" && -z $(prefix_problem "$prefix") ]]; then
    ((check_only)) && echo 'FFMPEG|current'
    echo "FFmpeg $version is current in $prefix"
    exit 0
fi

# nasm assembles FFmpeg's x86 code; it and the rest are needed only to build, never to use a current prefix.
missing=()
for tool in "$cc" make nasm curl tar xz sha256sum awk; do
    command -v "$tool" >/dev/null || missing+=("$tool")
done
if ((${#missing[@]})); then
    packages=()
    for tool in "${missing[@]}"; do packages+=("$(apt_package "$tool")"); done
    ((check_only)) && echo "FFMPEG|blocked|${missing[*]}"
    echo "Missing: ${missing[*]} (needed to build the pinned FFmpeg $version, which $prefix does not hold yet)"
    echo "Prepare: sudo bash packaging/linux/ci-deps.sh on the Ubuntu 20.04 release environment (docs/LINUX.md); on a development distribution: sudo apt-get install ${packages[*]}"
    exit 3
fi
if ((check_only)); then
    echo 'FFMPEG|build'
    exit 0
fi

mkdir -p -- "$work"
if ! echo "$sha256  $tarball" | sha256sum -c --status - 2>/dev/null; then
    echo "Fetching FFmpeg $version"
    curl -fsSL --retry 3 -o "$tarball.part" "$url"
    echo "$sha256  $tarball.part" | sha256sum -c --status - || { rm -f -- "$tarball.part"; die "$url does not match its pinned SHA-256"; }
    mv -f -- "$tarball.part" "$tarball"
fi

rm -rf -- "$work"/stage.*
stage=$(mktemp -d "$work/stage.XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
mkdir -- "$stage/src"
tar -xJf "$tarball" -C "$stage/src" --strip-components=1
echo "Building FFmpeg $version into $prefix"
# A neutral prefix and the relative source folder keep paths of this machine out of the libraries.
(cd "$stage" && src/configure --prefix=/ffmpeg --cc="$cc" "${options[@]}" >configure.log 2>&1) ||
    { tail -40 "$stage/configure.log" >&2; die 'FFmpeg configure failed'; }
grep -qx 'License: LGPL version 2.1 or later' "$stage/configure.log" || die 'FFmpeg was not configured as LGPL 2.1 or later'
grep -q '^#define HAVE_X86ASM 1' "$stage/config.h" || die 'FFmpeg was configured without its x86 assembly'
(cd "$stage" && make -j"$(nproc)" >make.log 2>&1) || { tail -40 "$stage/make.log" >&2; die 'FFmpeg build failed'; }
(cd "$stage" && make install DESTDIR="$stage/install" >install.log 2>&1) ||
    { tail -20 "$stage/install.log" >&2; die 'FFmpeg install failed'; }

staged=$stage/install/ffmpeg
sed -i "s|=/ffmpeg|=$prefix|g" "$staged"/lib/pkgconfig/*.pc
for name in "${names[@]}"; do
    ! grep -aqF -- "$work" "$staged/lib/lib$name.so" || die "lib$name.so names the build folder"
done
mkdir -p -- "$staged/share/licenses/ffmpeg"
cp -f -- "$stage/src/LICENSE.md" "$stage/src/COPYING.LGPLv2.1" "$staged/share/licenses/ffmpeg/"
printf '%s\n%s\n%s\n' "FFmpeg $version, $url (SHA-256 $sha256)" "./configure ${options[*]}" "$(nasm -v | head -n 1)" \
    >"$staged/share/licenses/ffmpeg/BUILD.txt"
problem=$(prefix_problem "$staged")
[[ -z $problem ]] || die "the staged FFmpeg is incomplete: $problem"
rm -rf -- "$prefix"
mkdir -p -- "$prefix"
cp -a -- "$staged/." "$prefix/"
printf '%s' "$expected" >"$stamp"
echo "FFmpeg $version installed in $prefix"
