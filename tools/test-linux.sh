#!/usr/bin/env bash
# The Linux test runner (tools\test.ps1 runs it through WSL): builds in build/cmake/linux and runs the headless tests.
# Neither the program nor any window is ever started.
#
#   tools/test-linux.sh [--full]           build the program and every test target, then run every suite but the contract
#   tools/test-linux.sh --parity           build and run only the parity contract (sotb-build-parity-tests)
#   tools/test-linux.sh --suite NAME...    build and run only these suites
#   tools/test-linux.sh --list             list the suites of the configured build folder
#
# Machine-readable lines: LINUX_TOOLCHAIN, LINUX_PLAN|count|names, LINUX_EXECUTABLE|name|state, LINUX_SUITE|name,
# LINUX_RESULT|name|state|detail, LINUX_SUMMARY, LINUX_FAIL|reason, LINUX_BLOCKED|reason. Exit 0 passes, 1 fails, 3 is
# blocked (a tool or a library is missing).
set -uo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=$root/build/cmake/linux
contract=sotb-build-parity-tests
program=shadow_of_the_beast
limit=${SOTB_LINUX_TEST_TIMEOUT:-900}
mode=full
wanted=()
while (($#)); do
    case $1 in
        --full) mode=full ;;
        --parity) mode=parity ;;
        --list) mode=list ;;
        --suite) mode=suite; shift; wanted=("$@"); break ;;
        -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

blocked() { echo "LINUX_BLOCKED|$1"; exit 3; }
fail() { echo "LINUX_FAIL|$1"; exit 1; }

log=$(mktemp) || fail 'no temporary file'
trap 'rm -f -- "$log"' EXIT
bash "$root/tools/build.sh" --configure >"$log" 2>&1
code=$?
if ((code)); then
    tail -n 40 "$log"
    ((code == 3)) && blocked "$(grep -a -m 1 '^Missing:' "$log" | tr '|' ' '). $(grep -a -m 1 '^Prepare:' "$log" | tr '|' ' ')"
    if grep -aqiE 'could not find|could not be found|not found' "$log"; then
        blocked 'a library the Linux build needs is not installed (see above)'
    fi
    fail 'the Linux build could not be configured'
fi
echo "LINUX_TOOLCHAIN|$("${CC:-clang}" --version | head -n 1)|$(cmake --version | head -n 1)"

# Regenerates the build files first, so the list is the one of the current CMake files.
ninja -C "$build_dir" build.ninja >"$log" 2>&1 || { tail -n 40 "$log"; fail 'CMake could not regenerate the Linux build'; }
list=$build_dir/generated/sotb_test_targets.txt
[[ -s $list ]] || fail 'CMake wrote no list of test targets'
mapfile -t all < <(tr -d '\r' <"$list" | sed '/^$/d')
[[ " ${all[*]} " == *" $contract "* ]] || fail "$contract is not a Linux test target"

case $mode in
    list) printf '%s\n' "${all[@]}"; exit 0 ;;
    parity) suites=("$contract") ;;
    suite) suites=("${wanted[@]}") ;;
    full)
        suites=()
        for name in "${all[@]}"; do [[ $name == "$contract" ]] || suites+=("$name"); done
        ;;
esac
((${#suites[@]})) || fail 'no suite to run'
for name in "${suites[@]}"; do
    [[ " ${all[*]} " == *" $name "* ]] || fail "unknown suite $name"
done

build_target() {
    if ninja -C "$build_dir" "$1" >"$log" 2>&1 && [[ -x $build_dir/bin/$1 ]]; then return 0; fi
    tail -n 40 "$log"
    return 1
}

if [[ $mode == parity ]]; then
    build_target "$contract" || fail 'the parity contract did not compile on Linux'
    "$build_dir/bin/$contract"
    exit $?
fi

failed=()
echo "LINUX_PLAN|${#suites[@]}|${suites[*]}"
if [[ $mode == full ]]; then
    if ! build_target "$program"; then
        echo "LINUX_EXECUTABLE|$program|FAIL|it did not build"
        failed+=("$program")
    elif [[ $(od -An -tx1 -j16 -N4 "$build_dir/bin/$program" | tr -d ' ') != 02003e00 ]]; then
        # ELF type EXEC, machine x86_64: the guest heap needs the fixed low base.
        echo "LINUX_EXECUTABLE|$program|FAIL|it is not a fixed-base x86_64 ELF program"
        failed+=("$program")
    else
        echo "LINUX_EXECUTABLE|$program|PASS"
    fi
    ninja -C "$build_dir" -k 0 "${suites[@]}" >/dev/null 2>&1
fi

export SDL_VIDEODRIVER=dummy SDL_VIDEO_DRIVER=dummy SDL_AUDIODRIVER=dummy SDL_AUDIO_DRIVER=dummy
export XDG_DATA_HOME=$build_dir/test-home/data XDG_CONFIG_HOME=$build_dir/test-home/config XDG_CACHE_HOME=$build_dir/test-home/cache
unset DISPLAY WAYLAND_DISPLAY APPIMAGE SOTB_PORTABLE_DIR
mkdir -p -- "$XDG_DATA_HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"
for name in "${suites[@]}"; do
    echo "LINUX_SUITE|$name"
    if ! build_target "$name"; then
        echo "LINUX_RESULT|$name|FAIL|it did not build; no older executable was run"
        failed+=("$name")
        continue
    fi
    begin=$SECONDS
    (cd "$build_dir/bin" && exec timeout --kill-after=10 "$limit" "./$name") >"$log" 2>&1 </dev/null
    code=$?
    if ((code)); then
        tail -n 40 "$log"
        reason="exit code $code"
        ((code == 124 || code == 137)) && reason="stopped after $limit s"
        echo "LINUX_RESULT|$name|FAIL|$reason"
        failed+=("$name")
    else
        echo "LINUX_RESULT|$name|PASS|$((SECONDS - begin)) s; $(tail -n 1 "$log" | tr '|' ' ')"
    fi
done
echo "LINUX_SUMMARY|suites=${#suites[@]}|failed=${#failed[@]}${failed[*]:+|${failed[*]}}"
((${#failed[@]} == 0))
