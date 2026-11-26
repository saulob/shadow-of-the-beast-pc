#!/usr/bin/env bash
# The Linux AppImage packaging contract, headless: tools/package-linux.sh's checks run on synthetic AppDirs built from
# tiny stand-in programs (never the real build, never a game, no network), AppRun with a stand-in program, the pinned
# runtime checks on synthetic runtimes, and the names and pins the updater and the release flow share.
# Prints PACKAGE_CASE|name|PASS or FAIL lines and ends with "N checks, M failures". Exit 0 passes, 1 fails, 3 is blocked.
set -uo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
for tool in cc readelf patchelf objcopy perl ldd od desktop-file-validate; do
    command -v "$tool" >/dev/null || { echo "PACKAGE_BLOCKED|$tool is missing"; exit 3; }
done
# shellcheck source=../tools/package-linux.sh
source "$root/tools/package-linux.sh"
set +e

checks=0 failures=0
pass() { checks=$((checks + 1)); echo "PACKAGE_CASE|$1|PASS"; }
fail() { checks=$((checks + 1)); failures=$((failures + 1)); echo "PACKAGE_CASE|$1|FAIL${2:+|$2}"; }
check() { local name=$1; shift; if "$@"; then pass "$name"; else fail "$name"; fi; }
refute() { local name=$1; shift; if "$@"; then fail "$name" 'expected a refusal'; else pass "$name"; fi; }

work=$(mktemp -d)
trap 'chmod -R u+w "$work" 2>/dev/null; rm -rf -- "$work"' EXIT
version=$(project_version "$root/CMakeLists.txt")

# Names, version and pins shared with the updater and the release flow.
check version-from-project [ -n "$version" ]
check version-matches-cmake grep -q "^project(shadow_of_the_beast_pc VERSION $version " "$root/CMakeLists.txt"
check appimage-name [ "$(appimage_name 1.2.3)" = ShadowOfTheBeastPC-v1.2.3-x86_64.AppImage ]
name=$(appimage_name 1.2.3)
prefix=${name%%1.2.3*} suffix=${name#*1.2.3}
check updater-asset-name grep -qF "\"$prefix\" + version.CoreString() + \"$suffix\"" "$root/launcher/update_config.cpp"
check updater-checksum-name grep -qF "\"$prefix\" + version.CoreString() + \"$suffix.sha256\"" "$root/launcher/update_config.cpp"
check release-script-name grep -qF '"ShadowOfTheBeastPC-v$version-x86_64.AppImage"' "$root/tools/package-release.ps1"
check release-notes-name grep -qF "\`$(appimage_name "$version")\`" "$root/docs/releases/v$version.md"
check release-notes-windows-name grep -qF "\`ShadowOfTheBeastPC-v$version-windows-x64.zip\`" "$root/docs/releases/v$version.md"
check release-notes-checksums bash -c 'grep -qF @WINDOWS_SHA256@ "$1" && grep -qF @LINUX_SHA256@ "$1"' _ "$root/docs/releases/v$version.md"
literal=$(grep -lF "$version" "$root"/tools/package-linux.sh "$root"/tools/package-release-linux.sh "$root"/tools/package-release.ps1 \
    "$root"/tools/build-appimage-runtime.sh "$root"/packaging/linux/* "$root"/packaging/README-linux.txt 2>/dev/null)
check no-literal-version [ -z "$literal" ]
. "$root/patches/appimage-runtime/baseline.env"
check runtime-pins bash -c '[[ $1 =~ ^[0-9a-f]{40}$ && $2 =~ ^[0-9a-f]{64}$ && $3 == https://github.com/AppImage/type2-runtime.git ]]' \
    _ "$RUNTIME_COMMIT" "$RUNTIME_SHA256" "$RUNTIME_REPO"
patch=$root/patches/appimage-runtime/0001-extract-and-run-when-fuse-fails.patch
check fuse-fallback-patch bash -c 'grep -qF "extract_and_run = true;" "$1" && grep -qF "goto run_extracted;" "$1" &&
    grep -qF "appimage_extracted_XXXXXX" "$1"' _ "$patch"
check patch-whitespace-attribute grep -qx 'patches/appimage-runtime/\*.patch -whitespace' "$root/.gitattributes"
check pinned-runtime-only grep -qF -- '--runtime-file "$runtime"' "$root/tools/package-linux.sh"
check ci-deps-baseline bash -c 'grep -qF "VERSION_ID == 20.04" "$1" && grep -qF "NASM_SHA256=" "$1" && grep -qw patchelf "$1" &&
    grep -qw desktop-file-utils "$1"' _ "$root/packaging/linux/ci-deps.sh"

refute glibc-order-newer newer_than 2.31 2.31
mirror_ok=1
for excluded in /.git /game /user /cache /logs /build /dist /out; do
    grep -qF -- "--exclude '$excluded'" "$root/tools/package-release-linux.sh" || { mirror_ok=0; echo "  the release mirror copies $excluded"; }
done
check release-mirror-excludes [ "$mirror_ok" = 1 ]
check release-baseline-distro bash -c 'grep -qF "\"\$ID \$VERSION_ID\" = \"ubuntu 20.04\"" "$1" && grep -qF "= \"glibc 2.31\"" "$1"' \
    _ "$root/tools/package-release.ps1"
check glibc-order-2.32 newer_than 2.32 2.31
check glibc-order-2.38 newer_than 2.38 2.31
refute glibc-order-2.4 newer_than 2.4 2.31
refute glibc-order-2.2.5 newer_than 2.2.5 2.31
check build-host-2.31 bash -c 'source "$1"; set +e; check_build_host 2.31' _ "$root/tools/package-linux.sh"
out=$( (check_build_host 2.38) 2>&1 ); status=$?
check build-host-2.38-refused bash -c '[[ $1 != 0 && $2 == *"Ubuntu 20.04"* ]]' _ "$status" "$out"

host_ok=1
for lib in libc.so.6 libm.so.6 libmvec.so.1 libnss_files.so.2 libvulkan.so.1 libGL.so.1 libEGL.so.1 libOpenGL.so.0 \
           libdrm.so.2 libgbm.so.1 libwayland-client.so.0 libX11.so.6 libxcb.so.1 libva.so.2 libvdpau.so.1 \
           libnvidia-glcore.so.550.54 libcuda.so.1 libOpenCL.so.1 libasound.so.2 libpipewire-0.3.so.0 libudev.so.1 libz.so.1; do
    is_host_lib "$lib" || { host_ok=0; echo "  $lib should come from the host"; }
done
for lib in libSDL3.so.0 libavcodec.so.62 libfreetype.so.6 libpng16.so.16 libstdc++.so.6 libselinux.so.1 libpcre2-8.so.0 \
           libcap.so.2 libcrypto.so.3 libfmt.so.9 libZydis.so.4.0 libxxhash.so.0; do
    is_host_lib "$lib" && { host_ok=0; echo "  $lib should be bundled"; }
done
check library-policy [ "$host_ok" = 1 ]

# Stand-in program and library: no libc, so they run on any glibc and need none of its versions.
cat > "$work/fake.c" <<'EOF'
int sotb_fake(void) { return 0; }
EOF
cat > "$work/prog.c" <<EOF
extern int sotb_fake(void);
__attribute__((used)) static const char about[] = "v$version   \xC2\xB7   ";
void _start(void) {
    __asm__ volatile("syscall" :: "a"(60), "D"(sotb_fake()));
    for (;;) {}
}
EOF
cat > "$work/runtime.c" <<'EOF'
void _start(void) {
    __asm__ volatile("syscall" :: "a"(60), "D"(0));
    for (;;) {}
}
EOF
cat > "$work/glibc.c" <<'EOF'
#include <stdio.h>
int sotb_print(void) { return puts("x"); }
EOF
(cd "$work" &&
    cc -shared -fPIC -nostdlib -Wl,-soname,libsotbfake.so.1 -o libsotbfake.so.1 fake.c &&
    cc -nostdlib -fno-pie -no-pie -O1 -o prog prog.c ./libsotbfake.so.1 -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
       -Wl,--disable-new-dtags -Wl,-rpath,'$ORIGIN/../lib' &&
    cc -nostdlib -fpie -pie -O1 -o prog-pie prog.c ./libsotbfake.so.1 -Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2 \
       -Wl,--disable-new-dtags -Wl,-rpath,'$ORIGIN/../lib' &&
    cc -static -nostdlib -fno-pie -no-pie -O1 -o runtime-base runtime.c && cc -shared -fPIC -o libsotbglibc.so.1 glibc.c) \
    >"$work/cc.log" 2>&1 || { cat "$work/cc.log"; echo "PACKAGE_BLOCKED|the stand-in programs did not compile"; exit 3; }

make_appdir() { # dir
    local dir=$1 doc=$1/$doc_dir lic=$1/$license_dir leaf
    mkdir -p "$dir/usr/bin/assets/launcher" "$dir/usr/lib/compat" "$dir/usr/share/applications" "$dir/usr/share/metainfo" \
             "$dir/usr/share/icons/hicolor/256x256/apps" "$doc" "$lic/ffmpeg" "$lic/SMAA" "$lic/shadPS4" "$lic/LibAtrac9"
    cp "$work/prog" "$dir/usr/bin/$program"
    cp "$work/libsotbfake.so.1" "$dir/usr/lib/"
    sed 's/@GLIBCXX_NEED@/0/g; s/@GCC_NEED@/0/g' "$root/packaging/linux/AppRun" > "$dir/AppRun"
    chmod 755 "$dir/AppRun" "$dir/usr/bin/$program"
    cp "$root/packaging/linux/$desktop_file" "$dir/$desktop_file"
    cp "$root/packaging/linux/$desktop_file" "$dir/usr/share/applications/$desktop_file"
    sed "s/@VERSION@/$version/; s/@DATE@/2026-11-26/" "$root/packaging/linux/$app_id.appdata.xml" > "$dir/$metainfo"
    sed "s/@VERSION@/$version/" "$root/packaging/README-linux.txt" > "$doc/README.txt"
    for leaf in "$icon.png" "usr/share/icons/hicolor/256x256/apps/$icon.png" usr/bin/assets/launcher/header-art.png \
                usr/bin/assets/launcher/sponsor-heart.png; do
        printf '\x89PNG\r\n\x1a\nimage' > "$dir/$leaf"
    done
    ln -s "$icon.png" "$dir/.DirIcon"
    for leaf in LICENSE.txt THIRD-PARTY-LICENSES.txt SYSTEM-PACKAGES.txt ffmpeg/COPYING.LGPLv2.1 ffmpeg/BUILD.txt \
                SMAA/LICENSE.txt shadPS4/LICENSE LibAtrac9/LICENSE; do
        echo 'licence text' > "$lic/$leaf"
    done
}

clean=$work/clean
make_appdir "$clean"
out=$(check_appdir "$clean" "$version" 2>&1); status=$?
check clean-appdir-passes [ "$status" = 0 ]
[[ $status == 0 ]] || echo "$out"
check glibc-max-reported bash -c '[[ $1 == *"GLIBC_MAX|program|none"* && $1 == *"GLIBC_MAX|libraries|none"* ]]' _ "$out"
check desktop-file-validates desktop-file-validate "$root/packaging/linux/$desktop_file"
if command -v appstreamcli >/dev/null; then
    check metainfo-validates appstreamcli validate --no-net "$clean/$metainfo"
fi

# Each case: a copy of the clean AppDir with one change, and the problem the check must report.
expect_problem() { # name expected-text mutation...
    local case_name=$1 expected=$2 dir=$work/case-$1 output
    shift 2
    rm -rf -- "$dir"
    cp -a "$clean" "$dir"
    (cd "$dir" && eval "$*") || { fail "$case_name" 'the mutation failed'; return; }
    output=$(check_appdir "$dir" "${case_version:-$version}" 2>&1)
    if [[ $? != 0 && $output == *"$expected"* ]]; then pass "$case_name"; else fail "$case_name" "expected '$expected'"; echo "$output" | tail -5; fi
    rm -rf -- "$dir"
}
utf16() { printf '%s' "$1" | sed 's/./&\x00/g'; }

expect_problem eboot 'forbidden name: usr/bin/eboot.bin' 'echo x > usr/bin/eboot.bin'
expect_problem game-folder 'forbidden name: game' 'mkdir -p game/prepared'
expect_problem prepared-build 'forbidden name' 'mkdir -p usr/share/CUSA03762-1.00'
expect_problem logs-folder 'forbidden name: logs' 'mkdir logs && echo x > logs/sotb.txt'
expect_problem user-settings 'forbidden name: user' 'mkdir user && echo x > user/notes.txt'
expect_problem settings-file 'forbidden name: usr/sotb.ini' 'echo x > usr/sotb.ini'
expect_problem bug-report-receipts 'forbidden name' "echo '{}' > $doc_dir/bug_reports.json"
expect_problem trophy-key 'forbidden name' 'echo x > usr/keys.json'
expect_problem ps4-package 'PS4 package: usr/share/game.dat2' "printf '\x7fCNT' > usr/share/game.dat2"
expect_problem ps4-self 'PS4 SELF: usr/share/module.x' "printf '\x4f\x15\x3d\x1d' > usr/share/module.x"
expect_problem cpk-archive 'CPK archive' "printf 'CPK data' > usr/share/movie.x"
expect_problem test-executable 'unexpected program or library: usr/bin/sotb-launcher-tests' "cp usr/bin/$program usr/bin/sotb-launcher-tests"
expect_problem object-file 'forbidden file type: usr/lib/launcher.o' 'echo x > usr/lib/launcher.o'
expect_problem debug-symbols 'forbidden file type' "echo x > usr/bin/$program.debug"
expect_problem crash-dump 'forbidden file type' 'echo x > usr/bin/crash.dmp'
expect_problem test-fixture 'forbidden file type' 'echo {} > usr/bin/bug-report-request.json'
expect_problem renderdoc 'forbidden file type' 'cp usr/lib/libsotbfake.so.1 usr/lib/librenderdoc.so'
expect_problem vulkan-layer 'forbidden file type' 'mkdir -p usr/share/vulkan && echo {} > usr/share/vulkan/VkLayer_obs.json'
expect_problem wrangler-state 'forbidden name' 'mkdir -p usr/.wrangler && echo x > usr/.wrangler/state'
expect_problem temporary-file 'forbidden file type' 'echo x > usr/bin/update.part'
expect_problem debug-marker "'DEBUG_SAULO'" "echo DEBUG_SAULO >> $doc_dir/README.txt"
expect_problem local-path-ascii "'VisualStudio'" "printf 'S:/VisualStudio/repos' >> $icon.png"
expect_problem local-path-utf16 "'VisualStudio'" "utf16 'C:\\VisualStudio\\repos' >> usr/bin/assets/launcher/header-art.png"
expect_problem checkout-path "'$root'" "echo '$root/build' >> $license_dir/LICENSE.txt"
expect_problem other-game-name "'God of War'" "echo 'God of War' >> $doc_dir/README.txt"
expect_problem access-token 'access token' "echo ghp_0123456789abcdefghijklmnopqrstuvwxyz >> $license_dir/LICENSE.txt"
expect_problem private-key 'private key' "printf -- '-----BEGIN PRIVATE KEY-----\nMIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQC7\n' >> $doc_dir/README.txt"
expect_problem runpath-program 'carries a DT_RUNPATH' "patchelf --remove-rpath usr/bin/$program && patchelf --set-rpath '\$ORIGIN/../lib' usr/bin/$program"
expect_problem build-rpath 'does not carry DT_RPATH' "patchelf --force-rpath --set-rpath '\$ORIGIN/../lib:/home/me/cache/deps' usr/bin/$program"
expect_problem runpath-library 'usr/lib/libsotbfake.so.1 carries a DT_RUNPATH' "patchelf --set-rpath '\$ORIGIN' usr/lib/libsotbfake.so.1"
expect_problem missing-library 'unresolved' 'rm usr/lib/libsotbfake.so.1'
expect_problem pie-program 'fixed-base EXEC' "cp '$work/prog-pie' usr/bin/$program"
expect_problem bundled-glibc 'glibc must come from the player' 'cp usr/lib/libsotbfake.so.1 usr/lib/libc.so.6'
expect_problem glibc-too-new 'needs glibc 9.9.9, newer than the 2.31 baseline' \
    "perl -pe 's/GLIBC_2\\.2\\.5/GLIBC_9.9.9/g' '$work/libsotbglibc.so.1' > usr/lib/libsotbglibc.so.1"
expect_problem compat-glibc-too-new 'needs glibc 9.9.9' \
    "perl -pe 's/GLIBC_2\\.2\\.5/GLIBC_9.9.9/g' '$work/libsotbglibc.so.1' > usr/lib/compat/libstdc++.so.6"
expect_problem missing-readme "missing: $doc_dir/README.txt" "rm $doc_dir/README.txt"
expect_problem missing-licence "missing: $license_dir/THIRD-PARTY-LICENSES.txt" "rm $license_dir/THIRD-PARTY-LICENSES.txt"
expect_problem missing-ffmpeg-licence "missing: $license_dir/ffmpeg/COPYING.LGPLv2.1" "rm $license_dir/ffmpeg/COPYING.LGPLv2.1"
expect_problem missing-metainfo "missing: $metainfo" "rm $metainfo"
expect_problem apprun-not-executable 'must be executable' 'chmod 644 AppRun'
expect_problem program-not-executable 'must be executable' "chmod 644 usr/bin/$program"
expect_problem apprun-placeholder 'unreplaced placeholder' "cp '$root/packaging/linux/AppRun' AppRun && chmod 755 AppRun"
expect_problem apprun-arguments 'does not start the program' "sed -i 's/ \"\$@\"\$//' AppRun"
expect_problem desktop-exec 'does not start AppRun' "sed -i 's/^Exec=AppRun\$/Exec=shadow_of_the_beast/' $desktop_file"
expect_problem link-outside 'unexpected symbolic link' 'ln -s /etc/passwd usr/share/passwd'
expect_problem source-art 'header-art-source.png must not ship' 'cp usr/bin/assets/launcher/header-art.png usr/bin/assets/launcher/header-art-source.png'
case_version=0.0.1 expect_problem stale-version 'was not built as version 0.0.1' ':'
case_version=0.0.1 expect_problem stale-metainfo 'no release entry for 0.0.1' ':'
cp "$work/libsotbglibc.so.1" "$clean/usr/lib/"
out=$(check_appdir "$clean" "$version" 2>&1); status=$?
newest=$(sed -n 's/^GLIBC_MAX|libraries|//p' <<<"$out")
old_ok=0
[[ $status == 0 && $newest =~ ^2\.[0-9.]+$ ]] && ! newer_than "$newest" 2.31 && old_ok=1
check old-glibc-library-passes [ "$old_ok" = 1 ]
rm -f -- "$clean/usr/lib/libsotbglibc.so.1"

# The pinned runtime: type 2 magic, static, and carrying the pinned source's version mark; the one at the front of an
# AppImage must equal it apart from the sections appimagetool fills.
mark="https://github.com/AppImage/type2-runtime/commit/${RUNTIME_COMMIT:0:7}+extract-fallback"
(cd "$work" && head -c 64 /dev/zero > upd && objcopy --add-section .upd_info=upd --set-section-flags .upd_info=noload,readonly runtime-base runtime &&
    printf 'AI\002' | dd of=runtime bs=1 seek=8 count=3 conv=notrunc status=none && printf '%s' "$mark" >> runtime) \
    >"$work/rt.log" 2>&1 || { cat "$work/rt.log"; fail runtime-fixture; }
check runtime-check-passes bash "$root/tools/build-appimage-runtime.sh" --check "$work/runtime"
cp "$work/runtime" "$work/runtime-unmarked" && perl -pi -e 's/extract-fallback/extract-fallbacX/' "$work/runtime-unmarked"
refute runtime-without-mark-refused bash "$root/tools/build-appimage-runtime.sh" --check "$work/runtime-unmarked"
cp "$work/prog" "$work/runtime-dynamic" && printf 'AI\002' | dd of="$work/runtime-dynamic" bs=1 seek=8 count=3 conv=notrunc status=none &&
    printf '%s' "$mark" >> "$work/runtime-dynamic"
refute runtime-dynamic-refused bash "$root/tools/build-appimage-runtime.sh" --check "$work/runtime-dynamic"
refute runtime-without-magic-refused bash "$root/tools/build-appimage-runtime.sh" --check "$work/prog"
# appimage_offset reads the section header table; the mark appended above lies past it, so the pinned file is the ELF.
pinned=$work/runtime-pinned
head -c "$(appimage_offset "$work/runtime")" "$work/runtime" > "$pinned"
upd=$(readelf -S -W "$pinned" | awk '{for (i = 1; i < NF - 3; i++) if ($i == ".upd_info") print $(i + 3)}')
cp "$pinned" "$work/image.AppImage" && printf 'update information' | dd of="$work/image.AppImage" bs=1 seek=$((16#$upd)) conv=notrunc status=none &&
    printf 'hsqs file system' >> "$work/image.AppImage"
check embedded-runtime-matches verify_embedded_runtime "$work/image.AppImage" "$pinned"
cp "$work/image.AppImage" "$work/other.AppImage" && printf 'X' | dd of="$work/other.AppImage" bs=1 seek=100 conv=notrunc status=none
refute embedded-runtime-differs verify_embedded_runtime "$work/other.AppImage" "$pinned"
refute embedded-runtime-size verify_embedded_runtime "$work/image.AppImage" "$work/runtime"
out=$( (verify_appimage "$work/image.AppImage") 2>&1 ); status=$?
check appimage-name-refused bash -c '[[ $1 != 0 && $2 == *"is not named"* ]]' _ "$status" "$out"

# AppRun: starts usr/bin/shadow_of_the_beast with every argument, and adds the fallback C++ libraries only for an
# older host. A stand-in program prints what it was given; nothing else is started.
apprun=$work/apprun
mkdir -p "$apprun/usr/bin" "$apprun/usr/lib/compat"
cat > "$apprun/usr/bin/$program" <<'EOF'
#!/bin/sh
printf 'arg=%s\n' "$@"
printf 'lib=%s\n' "${LD_LIBRARY_PATH:-}"
EOF
chmod 755 "$apprun/usr/bin/$program"
sed 's/@GLIBCXX_NEED@/0/g; s/@GCC_NEED@/0/g' "$root/packaging/linux/AppRun" > "$apprun/AppRun"
chmod 755 "$apprun/AppRun"
ln -s "$apprun/AppRun" "$work/AppRun-link"
out=$(env -u LD_LIBRARY_PATH "$work/AppRun-link" --launcher 'two words' '' 2>&1)
check apprun-arguments [ "$out" = $'arg=--launcher\narg=two words\narg=\nlib=' ]
sed 's/@GLIBCXX_NEED@/999/g; s/@GCC_NEED@/0/g' "$root/packaging/linux/AppRun" > "$apprun/AppRun"
out=$(LD_LIBRARY_PATH=/host/lib "$apprun/AppRun" --launcher 2>&1)
check apprun-compat-fallback [ "$out" = $'arg=--launcher\n'"lib=$apprun/usr/lib/compat:/host/lib" ]

echo "$checks checks, $failures failures"
((failures == 0))
