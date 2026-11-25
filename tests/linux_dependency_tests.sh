#!/usr/bin/env bash
# Linux dependency policy: tools/build-ffmpeg-linux.sh --check against fixture prefixes, with a PATH that holds exactly the
# tools each case allows. --check never downloads or builds. The rest reads the Linux scripts and tools/test.ps1.
set -uo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
script=$repo/tools/build-ffmpeg-linux.sh
failures=0
check() {
    if eval "$2"; then echo "  [ok]   $1"; else echo "  [FAIL] $1"; failures=$((failures + 1)); fi
}

fx=$(mktemp -d)
trap 'rm -rf -- "$fx"' EXIT
tools=$fx/bin
mkdir -p -- "$tools"
for tool in awk cat cut dirname grep mkdir realpath sha256sum tr; do
    printf '#!%s\nexec %q "$@"\n' "$BASH" "$(command -v "$tool")" >"$tools/$tool"
    chmod +x "$tools/$tool"
done
offer() { for tool in "$@"; do printf '#!%s\nexit 0\n' "$BASH" >"$tools/$tool"; chmod +x "$tools/$tool"; done; }
withdraw() { for tool in "$@"; do rm -f -- "$tools/$tool"; done; }

prefix=$fx/ffmpeg
complete_prefix() {
    rm -rf -- "$prefix"
    local name
    for name in avcodec avformat avutil swscale swresample; do
        mkdir -p -- "$prefix/lib/pkgconfig" "$prefix/include/lib$name"
        : >"$prefix/lib/lib$name.so.1"
        : >"$prefix/lib/pkgconfig/lib$name.pc"
        : >"$prefix/include/lib$name/$name.h"
    done
    mkdir -p -- "$prefix/share/licenses/ffmpeg"
    echo './configure --disable-everything --enable-shared' >"$prefix/share/licenses/ffmpeg/BUILD.txt"
    : >"$prefix/share/licenses/ffmpeg/LICENSE.md"
    : >"$prefix/share/licenses/ffmpeg/COPYING.LGPLv2.1"
    cat "$repo/gpu/ffmpeg.txt" "$script" | sha256sum | cut -d' ' -f1 | tr -d '\n' >"$prefix/share/licenses/ffmpeg/sotb-build.stamp"
}
probe() { output=$(CC=clang PATH=$tools "$BASH" "$script" --check "$prefix" "$fx/work" 2>&1); code=$?; }

complete_prefix
probe
check 'a current pinned FFmpeg needs no nasm, make or compiler' '[[ $code == 0 && $output == *"FFMPEG|current"* ]]'

echo stale >"$prefix/share/licenses/ffmpeg/sotb-build.stamp"
probe
check 'a stale stamp needs a build: nasm is required' '[[ $code == 3 && $output == *"FFMPEG|blocked|"*nasm* ]]'
check 'the missing nasm message points to the setup procedure' \
    '[[ $output == *"Missing: "*nasm*"FFmpeg"* && $output == *"Prepare: sudo bash packaging/linux/ci-deps.sh"* && $output == *"apt-get install"*nasm* ]]'

complete_prefix
rm -f -- "$prefix/lib/libswscale.so.1"
probe
check 'files of a current stamp are still checked: a missing library needs a build' '[[ $code == 3 && $output == *nasm* ]]'

rm -rf -- "$prefix"
probe
check 'no FFmpeg at all needs a build' '[[ $code == 3 && $output == *"FFMPEG|blocked|"* ]]'

offer clang make nasm curl tar xz
probe
check 'with every build tool present a build is reported, not done' '[[ $code == 0 && $output == *"FFMPEG|build"* && ! -e $fx/work ]]'
withdraw nasm
probe
check 'only nasm missing: blocked on nasm alone' '[[ $code == 3 && $output == *"FFMPEG|blocked|nasm"* ]]'
withdraw clang make curl tar xz

build=$(<"$repo/tools/build.sh")
runner=$(<"$repo/tools/test-linux.sh")
deps=$(<"$repo/packaging/linux/ci-deps.sh")
gate=$(<"$repo/tools/test.ps1")
check 'build.sh does not require nasm for every build' '! grep -qE "^for tool in .*nasm" <<<"$build"'
check 'build.sh stops with exit 3 when FFmpeg cannot be prepared' '[[ $build == *"build-ffmpeg-linux.sh\" \"\$ffmpeg\""*"|| exit \$?"* ]]'
check 'build.sh points to the setup script' '[[ $build == *"Prepare: sudo bash packaging/linux/ci-deps.sh"* ]]'
check 'test-linux.sh reports the setup procedure in LINUX_BLOCKED' '[[ $runner == *"^Missing:"*"^Prepare:"* ]]'
check 'the setup script installs nasm and make' 'grep -qE "^\s+clang-20 .*\bmake nasm\b" <<<"$deps"'
check 'the setup script keeps the Ubuntu 20.04 release baseline' '[[ $deps == *"\$VERSION_ID == 20.04"* ]]'
check 'no script installs packages by itself' \
    '! grep -qE "^\s*(sudo\s+)?(apt|apt-get|dnf|pacman|zypper)\s" "$repo/tools/build.sh" "$repo/tools/test-linux.sh" "$script" "$repo/tools/test.ps1"'
has() { grep -qF -- "$2" <<<"$1"; }
needs="Needs = @('build-parity', 'linux-tests', 'linux-parity')"
blocked_fails="'BLOCKED' { if (\$Full) { \$failed++ }"
blocked_kept="elseif (\$blocked) { \$outcome = Result 'BLOCKED'"
three_passed="\$unproven = @('build-parity', 'linux-tests', 'linux-parity'"
check 'platform-parity still needs build-parity, linux-tests and linux-parity' 'has "$gate" "$needs"'
check 'a BLOCKED group still fails -Full' 'has "$gate" "$blocked_fails"'
check 'linux-tests and linux-parity still turn LINUX_BLOCKED into BLOCKED' 'has "$gate" "$blocked_kept"'
check 'platform-parity passes only when the three groups passed' 'has "$gate" "$three_passed"'

echo "linux dependency tests: $failures failure(s)"
((failures == 0))
