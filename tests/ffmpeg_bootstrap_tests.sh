#!/usr/bin/env bash
# Headless tests of tools/build-ffmpeg.sh: the real script runs against fixture tarballs, pins and packages in a temporary
# folder whose name holds a space. Only configure, make and the installed files are faked; FFmpeg is never compiled.
set -uo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
repo=$(cd -- "$here/.." && pwd)
script=${FFMPEG_SCRIPT:-$repo/tools/build-ffmpeg.sh}
sandbox=$(mktemp -d "${TMPDIR:-/tmp}/ffmpeg bootstrap.XXXXXX")
trap 'rm -rf -- "$sandbox"' EXIT
checks=0 failures=0

check() {
    checks=$((checks + 1))
    if ! eval "$2"; then
        failures=$((failures + 1))
        echo "FAIL: $1"
    fi
}

new_fixture() {
    fx=$(mktemp -d "$sandbox/case.XXXXXX")
    mkdir -p "$fx/serve" "$fx/pkg/usr/bin" "$fx/ffmpeg-9.9.9"
    : >"$fx/ffmpeg-9.9.9/configure"
    echo license >"$fx/ffmpeg-9.9.9/LICENSE.md"
    echo lgpl >"$fx/ffmpeg-9.9.9/COPYING.LGPLv2.1"
    tar -cJf "$fx/serve/ffmpeg-9.9.9.tar.xz" -C "$fx" ffmpeg-9.9.9
    local sha tool
    sha=$(sha256sum "$fx/serve/ffmpeg-9.9.9.tar.xz" | cut -d' ' -f1)
    for tool in make nasm; do
        printf '%s' "$tool" >"$fx/pkg/usr/bin/$tool.exe"
        bsdtar --zstd -cf "$fx/serve/$tool-1-x86_64.pkg.tar.zst" -C "$fx/pkg" usr/bin/$tool.exe
    done
    cat >"$fx/ffmpeg.txt" <<PIN
version 9.9.9
url https://example.invalid/ffmpeg-9.9.9.tar.xz
sha256 $sha
configure --disable-everything
windows --enable-w32threads
tool-make https://example.invalid/make-1-x86_64.pkg.tar.zst $(sha256sum "$fx/serve/make-1-x86_64.pkg.tar.zst" | cut -d' ' -f1)
tool-nasm https://example.invalid/nasm-1-x86_64.pkg.tar.zst $(sha256sum "$fx/serve/nasm-1-x86_64.pkg.tar.zst" | cut -d' ' -f1)
PIN
    prefix=$fx/cache/ffmpeg/9.9.9/clang64
    work=$fx/cache/ffmpeg/9.9.9/work
}

# Runs the build in a subshell with the heavy steps replaced; the files calls, fetches and hooks count what ran.
run_build() {
    local under=${1:-$script}
    (
        export FFMPEG_PIN=$fx/ffmpeg.txt FFMPEG_LOCK_WAIT=2
        source "$under"
        fetch_file() { echo "$1" >>"$fx/fetches"; cp -- "${FETCH_SOURCE:-$fx/serve/$(basename -- "$1")}" "$2"; }
        ensure_tools() { echo tools >>"$fx/calls"; }
        make() { echo "GNU Make 4.4.1"; }
        nasm() { echo "NASM version 2.16.03"; }
        configure_build() { echo configure >>"$fx/calls"; }
        compile_build() { echo compile >>"$fx/calls"; [[ -z ${FAIL_COMPILE:-} ]] || die 'FFmpeg build failed'; }
        install_staging() {
            echo install >>"$fx/calls"
            local out=$stage/install/ffmpeg name
            mkdir -p "$out/bin" "$out/lib/pkgconfig" "$out/include"/{libavcodec,libavformat,libavutil,libswscale,libswresample}
            for name in avcodec avformat avutil swscale swresample; do
                echo "LGPL version 2.1 or later" >"$out/bin/$name-9.dll"
                echo "prefix=/ffmpeg" >"$out/lib/pkgconfig/lib$name.pc"
            done
            : >"$out/include/libavcodec/avcodec.h" && : >"$out/include/libavformat/avformat.h"
            : >"$out/include/libavutil/avutil.h" && : >"$out/include/libswscale/swscale.h"
            : >"$out/include/libswresample/swresample.h"
        }
        main "$prefix" "$work"
    ) >"$fx/out.log" 2>&1
}

calls() { cat "$fx/calls" 2>/dev/null | grep -c "^$1\$" || true; }
fetches() { [[ -f $fx/fetches ]] && wc -l <"$fx/fetches" | tr -d ' ' || echo 0; }
reset_counters() { rm -f -- "$fx/calls" "$fx/fetches"; }
stamp_of() { cat "$prefix/share/licenses/ffmpeg/sotb-build.stamp" 2>/dev/null; }

# A: no cache. Fetches and builds, publishes, stamps.
new_fixture
run_build; rc=$?
check "A: first build succeeds from nothing" '[[ $rc == 0 ]]'
check "A: tarball fetched once and kept" '[[ $(fetches) == 1 && -f $work/ffmpeg-9.9.9.tar.xz ]]'
check "A: every build step ran" '[[ $(calls configure) == 1 && $(calls compile) == 1 && $(calls install) == 1 ]]'
check "A: prefix published with its stamp" '[[ -n $(stamp_of) && -f $prefix/bin/avcodec-9.dll && -f $prefix/share/licenses/ffmpeg/BUILD.txt ]]'
check "A: pkg-config files name the published prefix" '! grep -q "=/ffmpeg" "$prefix/lib/pkgconfig/libavcodec.pc" && grep -q "prefix=" "$prefix/lib/pkgconfig/libavcodec.pc"'
check "A: no staging is left behind" '! compgen -G "$work/stage.*" >/dev/null'
check "A: the path with a space worked" '[[ $fx == *" "* ]]'

# Current stamp: reused, nothing runs.
reset_counters
run_build; rc=$?
check "current stamp: reused" '[[ $rc == 0 && $(calls install) == 0 && $(fetches) == 0 ]] && grep -q "is current in" "$fx/out.log"'

# Stale stamp: the rebuild path runs and the stamp is renewed.
printf old >"$prefix/share/licenses/ffmpeg/sotb-build.stamp"; reset_counters
run_build; rc=$?
check "stale stamp: rebuilt" '[[ $rc == 0 && $(calls install) == 1 && $(stamp_of) != old ]]'
check "valid tarball: reused without a download" '[[ $(fetches) == 0 ]]'

# Partial prefix (stamp kept, a DLL missing) and a non-LGPL record are not accepted.
rm -f "$prefix"/bin/avutil-*.dll; reset_counters
run_build; rc=$?
check "partial prefix: not accepted, rebuilt" '[[ $rc == 0 && $(calls install) == 1 && -f $prefix/bin/avutil-9.dll ]]'
echo "./configure --enable-gpl" >>"$prefix/share/licenses/ffmpeg/BUILD.txt"; reset_counters
run_build; rc=$?
check "GPL configuration: not accepted, rebuilt" '[[ $rc == 0 && $(calls install) == 1 ]] && ! grep -q -- "--enable-gpl" "$prefix/share/licenses/ffmpeg/BUILD.txt"'
rm -f "$prefix/share/licenses/ffmpeg/sotb-build.stamp"; reset_counters
run_build; rc=$?
check "valid files without a stamp: rebuilt" '[[ $rc == 0 && $(calls install) == 1 ]]'

# Changed pin and changed script.
printf '\n# changed\n' >>"$fx/ffmpeg.txt"; reset_counters
run_build; rc=$?
check "changed pin: rebuilt" '[[ $rc == 0 && $(calls install) == 1 ]]'
mkdir -p "$fx/copy/tools" "$fx/copy/gpu"
cp "$script" "$fx/copy/tools/build-ffmpeg.sh"; printf '\n# changed\n' >>"$fx/copy/tools/build-ffmpeg.sh"; reset_counters
run_build "$fx/copy/tools/build-ffmpeg.sh"; rc=$?
check "changed script: rebuilt" '[[ $rc == 0 && $(calls install) == 1 ]]'

# Failed staging: the published prefix stays intact.
printf old >"$prefix/share/licenses/ffmpeg/sotb-build.stamp"
before=$(cd "$prefix" && find . -type f | sort | xargs sha256sum | sha256sum); reset_counters
FAIL_COMPILE=1 run_build; rc=$?
after=$(cd "$prefix" && find . -type f | sort | xargs sha256sum | sha256sum)
check "failed build: reported" '[[ $rc != 0 ]] && grep -q "FFmpeg build failed" "$fx/out.log"'
check "failed build: published prefix intact" '[[ $before == $after ]]'
reset_counters
run_build; rc=$?
check "after a failed build: the next one recovers on its own" '[[ $rc == 0 && $(calls install) == 1 && $(fetches) == 0 ]]'
check "after a failed build: old staging is gone" '! compgen -G "$work/stage.*" >/dev/null'

# The state of the real failure: a complete extraction under work/build, no src, a valid tarball.
new_fixture
mkdir -p "$work/build/ffmpeg-9.9.9/libavcodec" "$work/stage.old/src" "$work/install/ffmpeg"
cp "$fx/serve/ffmpeg-9.9.9.tar.xz" "$work/"
echo x >"$work/build/ffmpeg-9.9.9/libavcodec/bsf.c"
chmod a-w "$work/stage.old/src"
run_build; rc=$?
check "partial work and preexisting src: no rename, no failure" '[[ $rc == 0 && $(fetches) == 0 ]]'
check "partial work: removed automatically" '[[ ! -e $work/build && ! -e $work/install ]]'
check "the script never renames a directory" '! grep -nE "(^|[;&|[:space:]])mv[[:space:]]" "$script" | grep -q .'

# Invalid tarball: replaced from the pinned source; wrong bytes from the source are rejected and not kept.
new_fixture
mkdir -p "$work"; echo corrupt >"$work/ffmpeg-9.9.9.tar.xz"
run_build; rc=$?
check "invalid tarball: rejected and fetched again" '[[ $rc == 0 && $(fetches) == 1 ]] && sha256sum -c --status <(echo "$(sed -n "s/^sha256 //p" "$fx/ffmpeg.txt")  $work/ffmpeg-9.9.9.tar.xz")'
new_fixture
echo wrong >"$fx/wrong.bin"
FETCH_SOURCE=$fx/wrong.bin run_build; rc=$?
check "wrong bytes from the source: build stops" '[[ $rc != 0 ]] && grep -q "does not match the SHA-256" "$fx/out.log" && [[ ! -f $work/ffmpeg-9.9.9.tar.xz && ! -e $prefix ]]'
new_fixture
FETCH_SOURCE=$fx/missing.bin run_build; rc=$?
check "download failure: says the pinned download failed, asks for nothing to install" 'grep -q "downloading FFmpeg 9.9.9 failed" "$fx/out.log" && ! grep -qiE "install|pacman|setup\.ps1" "$fx/out.log"'

# Offline with a validated tarball.
new_fixture
mkdir -p "$work"; cp "$fx/serve/ffmpeg-9.9.9.tar.xz" "$work/"
FETCH_SOURCE=$fx/missing.bin run_build; rc=$?
check "offline with a valid tarball: builds without the network" '[[ $rc == 0 && $(fetches) == 0 ]]'

# Locks: a finished build's lock is reclaimed, a running one is respected.
new_fixture
mkdir -p "$prefix.lock"; echo 999999 >"$prefix.lock/pid"
run_build; rc=$?
check "stale lock: reclaimed" '[[ $rc == 0 && ! -e $prefix.lock ]]'
new_fixture
sleep 60 & holder=$!
mkdir -p "$prefix.lock"; echo "$holder" >"$prefix.lock/pid"
run_build; rc=$?
check "live lock: the second build waits, then stops without touching anything" '[[ $rc != 0 && ! -e $prefix && -d $prefix.lock ]] && grep -q "another build is producing" "$fx/out.log"'
kill "$holder" 2>/dev/null; wait "$holder" 2>/dev/null
run_build; rc=$?
check "live lock released: the build proceeds" '[[ $rc == 0 ]]'

# Pinned make and nasm: unpacked once, verified, resolved from the cache and not from the machine.
new_fixture
tools_check() {
    (
        export FFMPEG_PIN=$fx/ffmpeg.txt
        source "$script"
        prefix=$fx/cache/ffmpeg/9.9.9/clang64 work=$fx/w
        mkdir -p "$work"
        load_pins
        fetch_file() { echo "$1" >>"$fx/fetches"; cp -- "$fx/serve/$(basename -- "$1")" "$2"; }
        make() { echo "GNU Make 4.4.1"; }
        nasm() { echo "NASM version 2.16.03"; }
        tool_dirs=()
        ensure_tool make; ensure_tool nasm
        echo "${tool_dirs[*]}"
    )
}
reset_counters
out=$(tools_check 2>&1); rc=$?
check "tools: fetched, verified, unpacked into the cache" '[[ $rc == 0 && $(fetches) == 2 && -f $fx/cache/ffmpeg/tools/make-1-x86_64/make.exe && -f $fx/cache/ffmpeg/tools/nasm-1-x86_64/nasm.exe ]]'
reset_counters
out=$(tools_check 2>&1); rc=$?
check "tools: cached copies are reused without a download" '[[ $rc == 0 && $(fetches) == 0 ]]'
rm -f "$fx/cache/ffmpeg/tools/make-1-x86_64/sha256"; reset_counters
out=$(tools_check 2>&1); rc=$?
check "tools: an unfinished unpack is redone" '[[ $rc == 0 && -f $fx/cache/ffmpeg/tools/make-1-x86_64/sha256 ]]'
sed -i 's|^tool-nasm \(\S*\) .*|tool-nasm \1 0000000000000000000000000000000000000000000000000000000000000000|' "$fx/ffmpeg.txt"
rm -rf "$fx/cache/ffmpeg/tools/nasm-1-x86_64" "$fx/cache/ffmpeg/tools/downloads/nasm-1-x86_64.pkg.tar.zst"
out=$(tools_check 2>&1); rc=$?
check "tools: a package that does not match its pin is rejected" '[[ $rc != 0 ]] && grep -q "does not match" <<<"$out"'

# No manual dependency instruction anywhere in the FFmpeg flow.
sync_function=$(sed -n '/^function Sync-FFmpeg/,/^}/p' "$repo/tools/_common.ps1")
check "build-ffmpeg.sh asks for no manual installation" '! grep -qiE "pacman|setup\.ps1|install (make|nasm|ffmpeg)|missing (make|nasm)" "$script"'
check "Sync-FFmpeg asks for no manual installation" '! grep -qiE "pacman|setup\.ps1|install (make|nasm|ffmpeg)|missing (make|nasm)" <<<"$sync_function"'
check "make and nasm are not toolchain prerequisites" '! grep -E "^\\\$script:(BuildTools|Clang64Packages)" -A3 "$repo/tools/_common.ps1" | grep -qiwE "make|nasm"'

echo "ffmpeg bootstrap: $checks checks, $failures failures"
[[ $failures == 0 ]]
