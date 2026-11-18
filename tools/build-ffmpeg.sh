#!/usr/bin/env bash
# Builds the FFmpeg pinned in gpu/ffmpeg.txt into PREFIX (LGPL, MSYS2 CLANG64) and publishes it only after a staged build
# passed validation. tools\build.ps1 runs it; nothing is rebuilt while PREFIX holds the build of the current pins.
#
#   tools/build-ffmpeg.sh PREFIX [WORK]     WORK keeps the validated tarball; staging folders live and die inside it
#
# Layout: WORK/<tarball> (kept), WORK/stage.* (disposable), PREFIX (published), PREFIX.lock, ../tools (pinned make, nasm).
set -euo pipefail

self=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/$(basename -- "${BASH_SOURCE[0]}")
root=$(cd -- "$(dirname -- "$self")/.." && pwd)
die() { echo "ERROR: $*" >&2; exit 1; }
to_unix() { if command -v cygpath >/dev/null; then cygpath -u "$1"; else printf '%s' "$1"; fi; }
to_mixed() { if command -v cygpath >/dev/null; then cygpath -m "$1"; else printf '%s' "$1"; fi; }
discard() { rm -rf -- "$1" 2>/dev/null || echo "  Could not remove $1 yet (a scanner may still hold it); the next build retries."; }

pin=${FFMPEG_PIN:-$root/gpu/ffmpeg.txt}
field() { awk -v key="$1" '$1 == key { $1 = ""; sub(/^ /, ""); print }' "$pin"; }

load_pins() {
    version=$(field version) url=$(field url) sha256=$(field sha256)
    [[ -n $version && $url == https://* && $sha256 =~ ^[0-9a-f]{64}$ ]] || die "$pin needs version, an https url and sha256"
    read -r -a options <<<"$(field configure) $(field windows)"
    cc=${CC:-clang}
    licenses=$prefix/share/licenses/ffmpeg
    stamp=$licenses/sotb-build.stamp
    tools=$(dirname -- "$(dirname -- "$prefix")")/tools
    tarball=$work/ffmpeg-$version.tar.xz
    lock_dir=$prefix.lock
}

expected_stamp() { cat "$pin" "$self" | sha256sum | cut -d' ' -f1; }

# What is wrong with the FFmpeg in $1, empty when it is complete and LGPL.
prefix_problem() {
    local dir=$1 name lib
    for name in avcodec avformat avutil swscale swresample; do
        compgen -G "$dir/bin/$name-*.dll" >/dev/null || { echo "bin/$name-*.dll is missing"; return; }
        compgen -G "$dir/lib/pkgconfig/lib$name.pc" >/dev/null || { echo "lib$name.pc is missing"; return; }
    done
    for name in libavcodec/avcodec.h libavformat/avformat.h libavutil/avutil.h libswscale/swscale.h libswresample/swresample.h; do
        [[ -f $dir/include/$name ]] || { echo "include/$name is missing"; return; }
    done
    for name in BUILD.txt LICENSE.md COPYING.LGPLv2.1; do
        [[ -f $dir/share/licenses/ffmpeg/$name ]] || { echo "share/licenses/ffmpeg/$name is missing"; return; }
    done
    ! grep -qE -- '--enable-(gpl|nonfree)' "$dir/share/licenses/ffmpeg/BUILD.txt" || { echo "BUILD.txt names a non-LGPL configuration"; return; }
    for lib in "$dir"/bin/av*.dll "$dir"/bin/sw*.dll; do
        grep -aqF 'LGPL version 2.1 or later' "$lib" || { echo "$(basename "$lib") is not an LGPL build"; return; }
        ! grep -aqF -- '--enable-gpl' "$lib" || { echo "$(basename "$lib") was configured with --enable-gpl"; return; }
    done
}

is_current() {
    [[ -f $stamp && $(<"$stamp") == "$(expected_stamp)" ]] || return 1
    [[ -z $(prefix_problem "$prefix") ]]
}

release_lock() { [[ ${lock_held:-} ]] && rm -rf -- "$lock_dir"; lock_held=; return 0; }

try_lock() {
    mkdir -p -- "$(dirname -- "$lock_dir")"
    mkdir -- "$lock_dir" 2>/dev/null || return 1
    echo $$ >"$lock_dir/pid"
    lock_held=1
}

lock_is_stale() {
    local holder
    holder=$(cat "$lock_dir/pid" 2>/dev/null || true)
    if [[ -n $holder ]]; then ! kill -0 "$holder" 2>/dev/null; else [[ -n $(find "$lock_dir" -maxdepth 0 -mmin +1 2>/dev/null) ]]; fi
}

acquire_lock() {
    local waited=0 limit=${FFMPEG_LOCK_WAIT:-3600}
    until try_lock; do
        if lock_is_stale; then
            echo "Removing the lock of a build that is no longer running"
            rm -rf -- "$lock_dir"
            continue
        fi
        ((waited < limit)) || die "another build is producing this FFmpeg ($lock_dir); run the build again when it ends"
        ((waited > 0)) || echo "Waiting for the other build of FFmpeg to finish"
        sleep 2
        waited=$((waited + 2))
    done
    trap release_lock EXIT
}

# Staging folders of earlier attempts, and the folders earlier versions of this script used.
clean_work() {
    local leftover
    for leftover in "$work"/stage.* "$work/build" "$work/install" "$work/ffmpeg-$version" "$work"/*.part; do
        [[ -e $leftover ]] && discard "$leftover"
    done
    return 0
}

tidy_when_idle() {
    compgen -G "$work/stage.*" >/dev/null || compgen -G "$work/build" >/dev/null || return 0
    if try_lock; then clean_work; release_lock; fi
}

fetch_file() { curl -fsSL --retry 3 -o "$2" "$1"; }

# Validated copy of url in dest; downloads only when dest is absent or does not match.
ensure_download() {
    local source_url=$1 expected=$2 dest=$3 label=$4
    if [[ -f $dest ]] && echo "$expected  $dest" | sha256sum -c --status - 2>/dev/null; then return 0; fi
    rm -f -- "$dest"
    mkdir -p -- "$(dirname -- "$dest")"
    echo "Fetching $label"
    fetch_file "$source_url" "$dest" || { rm -f -- "$dest"; die "downloading $label failed ($source_url); check the connection and run the build again"; }
    echo "$expected  $dest" | sha256sum -c --status - || { rm -f -- "$dest"; die "$source_url does not match the SHA-256 pinned in $pin"; }
}

# make and nasm come from pinned MSYS2 packages kept beside the FFmpeg cache, never from whatever the machine has.
ensure_tool() {
    local name=$1 spec tool_url tool_sha dir
    spec=$(field "tool-$name")
    read -r tool_url tool_sha <<<"$spec"
    [[ $tool_url == https://* && $tool_sha =~ ^[0-9a-f]{64}$ ]] || die "$pin needs tool-$name <https url> <sha256>"
    dir=$tools/$(basename -- "$tool_url" .pkg.tar.zst)
    if [[ -x $dir/$name.exe && -f $dir/sha256 && $(<"$dir/sha256") == "$tool_sha" ]]; then
        tool_dirs+=("$dir")
        return 0
    fi
    ensure_download "$tool_url" "$tool_sha" "$tools/downloads/$(basename -- "$tool_url")" "$name"
    local scratch
    scratch=$(mktemp -d "$tools/extract.XXXXXX")
    bsdtar -xf "$tools/downloads/$(basename -- "$tool_url")" -C "$scratch" "usr/bin/$name.exe" || die "$name could not be unpacked"
    rm -rf -- "$dir"
    mkdir -p -- "$dir"
    cp -f -- "$scratch/usr/bin/$name.exe" "$dir/$name.exe"
    printf '%s' "$tool_sha" >"$dir/sha256"
    discard "$scratch"
    tool_dirs+=("$dir")
}

ensure_tools() {
    tool_dirs=()
    mkdir -p -- "$tools"
    ensure_tool make
    ensure_tool nasm
    local dir
    for dir in "${tool_dirs[@]}"; do PATH="$dir:$PATH"; done
    export PATH
    make --version | head -n 1 | grep -q 'GNU Make' || die 'the pinned make does not run'
    nasm -v | grep -q 'NASM version' || die 'the pinned nasm does not run'
}

extract_source() {
    mkdir -p -- "$stage/src"
    tar -xJf "$tarball" -C "$stage/src" --strip-components=1
    [[ -f $stage/src/configure ]] || die "$tarball does not hold the FFmpeg sources"
}

# The libraries carry their configure line and source file names: a neutral install prefix and the relative path "src"
# keep the staging folder out of them.
configure_build() {
    (cd "$stage" && src/configure --prefix=/ffmpeg --cc="$cc" "${options[@]}" >configure.log 2>&1) ||
        { tail -40 "$stage/configure.log" >&2; die 'FFmpeg configure failed'; }
    grep -qx 'License: LGPL version 2.1 or later' "$stage/configure.log" || die 'FFmpeg was not configured as LGPL 2.1 or later'
}

compile_build() {
    (cd "$stage" && make -j"$(nproc)" >make.log 2>&1) || { tail -40 "$stage/make.log" >&2; die 'FFmpeg build failed'; }
}

install_staging() {
    (cd "$stage" && make install DESTDIR="$stage/install" >install.log 2>&1) ||
        { tail -20 "$stage/install.log" >&2; die 'FFmpeg install failed'; }
}

# pkgconf and clang are Windows programs: they need the prefix as S:/..., and no library may name a staging folder.
finish_staged_install() {
    local staged=$stage/install/ffmpeg lib needle
    sed -i "s|=/ffmpeg|=$(to_mixed "$prefix")|g" "$staged"/lib/pkgconfig/libav*.pc "$staged"/lib/pkgconfig/libsw*.pc
    ! grep -qE '(=|-[IL])/ffmpeg(/|$)' "$staged"/lib/pkgconfig/*.pc || die 'pkg-config files still name /ffmpeg'
    for lib in "$staged"/bin/av*.dll "$staged"/bin/sw*.dll; do
        for needle in "$stage" "$(to_mixed "$stage")" "$work" "$(to_mixed "$work")"; do
            ! grep -aqF -- "$needle" "$lib" || die "$lib names the build folder $needle"
        done
    done
    mkdir -p -- "$staged/share/licenses/ffmpeg"
    cp -f -- "$stage/src/LICENSE.md" "$stage/src/COPYING.LGPLv2.1" "$staged/share/licenses/ffmpeg/"
    printf '%s\n%s\n%s\n%s\n' "FFmpeg $version, $url (SHA-256 $sha256)" "./configure ${options[*]}" \
        "$(make --version | head -n 1)" "$(nasm -v | head -n 1)" >"$staged/share/licenses/ffmpeg/BUILD.txt"
}

# The stamp is written last: a prefix without it, or with a missing file, is never accepted.
publish() {
    local staged=$stage/install/ffmpeg problem
    problem=$(prefix_problem "$staged" || true)
    [[ -z $problem ]] || die "the staged FFmpeg is incomplete: $problem"
    rm -f -- "$stamp"
    discard "$prefix"
    mkdir -p -- "$prefix"
    cp -a -- "$staged/." "$prefix/"
    problem=$(prefix_problem "$prefix" || true)
    [[ -z $problem ]] || die "the published FFmpeg is incomplete: $problem"
    mkdir -p -- "$licenses"
    printf '%s' "$(expected_stamp)" >"$stamp"
}

build_ffmpeg() {
    clean_work
    ensure_download "$url" "$sha256" "$tarball" "FFmpeg $version"
    ensure_tools
    stage=$(mktemp -d "$work/stage.XXXXXX")
    echo "Building FFmpeg $version into $prefix"
    extract_source
    configure_build
    compile_build
    install_staging
    finish_staged_install
    publish
    discard "$stage"
    echo "FFmpeg $version installed in $prefix"
}

main() {
    prefix=$(to_unix "${1:?usage: tools/build-ffmpeg.sh PREFIX [WORK]}")
    work=$(to_unix "${2:-$(dirname -- "$prefix")/work}")
    mkdir -p -- "$work"
    load_pins
    if is_current; then
        tidy_when_idle
        echo "FFmpeg $version is current in $prefix"
        return 0
    fi
    for tool in curl tar xz sha256sum bsdtar awk sed grep "$cc"; do
        command -v "$tool" >/dev/null || die "$tool is missing from the MSYS2 CLANG64 environment"
    done
    acquire_lock
    if is_current; then
        echo "FFmpeg $version is current in $prefix"
        return 0
    fi
    build_ffmpeg
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then main "$@"; fi
