#!/usr/bin/env bash
# Packages the tested Linux build as dist/ShadowOfTheBeastPC-v<version>-x86_64.AppImage and its .sha256. Local only:
# nothing is published, tagged or uploaded, and the program is never started. The Linux counterpart of tools\package.ps1.
#
#   tools/package-linux.sh                         build with tools/build.sh, then package
#   tools/package-linux.sh --skip-build            package the existing build/cmake/linux/bin/shadow_of_the_beast
#   tools/package-linux.sh --out-dir DIR           write the AppImage, .sha256 and symbols to DIR instead of dist/
#   tools/package-linux.sh --verify DIR            check an AppDir
#   tools/package-linux.sh --verify-appimage FILE  extract an AppImage (never running the program) and check it all
#   tools/package-linux.sh --extract-program FILE OUT  copy usr/bin/shadow_of_the_beast out of an AppImage with unsquashfs
#
# Release AppImages are built on Ubuntu 20.04 (packaging/linux/ci-deps.sh): no shipped ELF may need a glibc newer than
# 2.31. GPU drivers, the Vulkan loader, X11/Wayland, audio and glibc always come from the player's system.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
max_glibc=2.31
program=shadow_of_the_beast
app_id=io.github.saulob.shadow_of_the_beast_pc
desktop_file=$app_id.desktop
metainfo=usr/share/metainfo/$app_id.appdata.xml
icon=shadow-of-the-beast-pc
doc_dir=usr/share/doc/ShadowOfTheBeastPC
license_dir=usr/share/licenses/ShadowOfTheBeastPC
appimagetool_version=1.9.1
appimagetool_sha256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0

# Fallback copies, used only when the system's are older than the build's (packaging/linux/AppRun).
compat_libs='^(libstdc\+\+|libgcc_s)\.so'
glibc_parts='^(ld-linux.*|libc|libm|libdl|libpthread|librt|libresolv|libutil|libmvec|libanl|libnsl|libBrokenLocale|libthread_db|libnss_.*)\.so'
forbidden_names='^(game|user|logs|cache|build|prepared|extracted|image|sce_sys|sce_module|savedata|trophy|shader|eboot\.bin|param\.sfo|sotb\.ini|boot\.bin|content\.bin|image\.json|imports\.json|keys\.json|bug_reports\.json|symbols|trg-updater\.log|\.git|\.wrangler|\.dev\.vars|node_modules|squashfs-root|appimage_extracted_.*|CUSA[0-9]+.*)$'
forbidden_patterns='\.(pkg|pdb|debug|log|dmp|ini|bin|self|sprx|prx|elf|cpk|pak|usm|trp|sfo|json|spv|pipeline_cache|o|obj|a|lib|tmp|bak|part|sav|dat|cache|py|ps1|sh|c|cpp|h|cmake|ninja|stamp)$|renderdoc|^VkLayer|^trg-update-'

# Libraries the player's system provides. The graphics, video and compute driver stacks (Mesa RADV on a Steam Deck,
# NVIDIA, VA-API, VDPAU) must never be shadowed by a bundled copy. Ordinary libraries (libselinux, libpcre2, libcap) are
# bundled when something needs them: SteamOS lacks libselinux.
is_host_lib() {
    case "$1" in
        linux-vdso.so*|ld-linux-*.so*|libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*|libresolv.so.*|libutil.so.*|libnsl.so.*|libcrypt.so.*|libanl.so.*)
            return 0 ;;
        libGL.so.*|libGLX.so.*|libGLdispatch.so.*|libEGL.so.*|libGLESv2.so.*|libvulkan.so.*|libdrm*.so.*|libgbm.so.*|libwayland-*.so.*)
            return 0 ;;
        libOpenGL.so.*|libXrender.so.*|libva*.so.*|libvdpau.so.*|libnvidia-*.so.*|libcuda.so.*|libOpenCL.so.*|libmvec.so.*|libnss_*.so.*)
            return 0 ;;
        libX11.so.*|libX11-xcb.so.*|libXext.so.*|libXau.so.*|libXdmcp.so.*|libxcb*.so.*|libXfixes.so.*|libXcursor.so.*|libXrandr.so.*|libXi.so.*|libXinerama.so.*|libXss.so.*|libXxf86vm.so.*|libxkbcommon*.so.*)
            return 0 ;;
        libasound.so.*|libpulse*.so.*|libpipewire-*.so.*|libdbus-1.so.*|libudev.so.*|libsystemd.so.*|libfontconfig.so.*|libexpat.so.*|libuuid.so.*|libgpg-error.so.*|libgcrypt.so.*|liblz4.so.*|liblzma.so.*|libz.so.*)
            return 0 ;;
        *) return 1 ;;
    esac
}

die() { echo "ERROR: $*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }

project_version() { # CMakeLists.txt
    sed -nE 's/^project\(shadow_of_the_beast_pc VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$1" | tr -d '\r'
}
# The release asset the updater downloads (launcher/update_config.cpp).
appimage_name() { printf 'ShadowOfTheBeastPC-v%s-x86_64.AppImage' "$1"; }

newer_than() { [[ $1 != "$2" && $(printf '%s\n' "$1" "$2" | sort -V | tail -1) == "$1" ]]; }
# The build host's glibc is the oldest the AppImage runs on; every shipped ELF is checked again after staging.
check_build_host() { # glibc-version
    [[ $1 =~ ^[0-9]+\.[0-9]+$ ]] || die "Cannot tell this system's glibc version ($1)"
    if newer_than "$1" "$max_glibc"; then
        die "This system has glibc $1. Release AppImages are built on Ubuntu 20.04 (glibc $max_glibc): see packaging/linux/ci-deps.sh."
    fi
}
is_elf() { [[ $(head -c 4 -- "$1" | od -An -tx1 | tr -d ' \n') == 7f454c46 ]]; }
# The GLIBC_ versions in the version-needs section: the dynamic loader refuses to start an ELF without every one.
newest_glibc() {
    local needs
    needs=$(readelf --version-info -W -- "$1") || return 1
    awk '/^Version needs section/ {needs = 1; next} /^Version / {needs = 0} needs' <<<"$needs" |
        { grep -oE 'Name: GLIBC_[0-9]+(\.[0-9]+)*' || true; } | sed 's/^Name: GLIBC_//' | sort -V | tail -1
}

# One PCRE alternation that finds every needle case-insensitively as ASCII and as UTF-16LE text.
needle_pattern() {
    local needle i parts=()
    for needle in "$@"; do
        [[ -n $needle ]] || continue
        parts+=("\\Q$needle\\E")
        local wide=
        for ((i = 0; i < ${#needle}; i++)); do wide+="\\Q${needle:i:1}\\E\\x00"; done
        parts+=("$wide")
    done
    local IFS='|'
    printf '(?i)(%s)' "${parts[*]}"
}
local_needles() {
    printf '%s\n' "$root" "${HOME:+$HOME/}" VisualStudio ClaudeTemp Decomp_Downloads referencias-ps4 DEBUG_SAULO \
        build/cmake 'build\cmake' cache/deps 'Users/SauloB' 'Users\SauloB' /mnt/s/ gow3 'God of War' sons_of_sparta \
        alienation dragons_crown odin_sphere
}

# Checks a staged or extracted AppDir; with an AppImage file, its runtime's glibc too. Prints GLIBC_MAX|group|version.
check_appdir() { # dir [version] [appimage]
    local dir version=${2:-} appimage=${3:-} problems=0 item rel name magic leaf lib line target
    dir=$(readlink -f -- "$1")
    local -a needles files=()
    problem() { echo "  PROBLEM: $*" >&2; problems=$((problems + 1)); }
    mapfile -t needles < <(local_needles)
    local pattern
    pattern=$(needle_pattern "${needles[@]}")
    while IFS= read -r -d '' item; do
        rel=${item#"$dir"/}
        name=$(basename -- "$item")
        [[ $name =~ $forbidden_names ]] && problem "forbidden name: $rel"
        [[ $name =~ $glibc_parts ]] && problem "glibc must come from the player's system: $rel"
        if [[ -L $item ]]; then
            target=$(readlink -f -- "$item" || true)
            [[ $rel == .DirIcon && $target == "$dir/$icon.png" ]] || problem "unexpected symbolic link: $rel"
            continue
        fi
        [[ -f $item ]] || continue
        files+=("$item")
        if [[ $rel != "$license_dir"/* && $rel != "$doc_dir"/* && $name =~ $forbidden_patterns ]]; then
            problem "forbidden file type: $rel"
        fi
        magic=$(head -c 4 -- "$item" | od -An -tx1 | tr -d ' \n')
        case $magic in
            7f434e54) problem "PS4 package: $rel" ;;
            4f153d1d) problem "PS4 SELF: $rel" ;;
            43504b20) problem "CPK archive: $rel" ;;
            7f454c46) [[ $rel == "usr/bin/$program" || $rel =~ ^usr/lib/(compat/)?[^/]+\.so(\.[0-9]+)*$ ]] ||
                problem "unexpected program or library: $rel" ;;
        esac
        if LC_ALL=C grep -a -q -P -- "$pattern" "$item"; then
            local needle
            for needle in "${needles[@]}"; do
                [[ -n $needle ]] && LC_ALL=C grep -a -q -P -- "$(needle_pattern "$needle")" "$item" &&
                    { problem "local path or stale name '$needle' in $rel"; break; }
            done
        fi
        LC_ALL=C grep -a -q -z -P -- '-----BEGIN [A-Z ]*PRIVATE KEY-----\n[A-Za-z0-9+/=]{40}' "$item" &&
            problem "private key in $rel"
        LC_ALL=C grep -a -q -E -- 'gh[pousr]_[A-Za-z0-9]{36}|github_pat_[A-Za-z0-9_]{40}|AKIA[0-9A-Z]{16}' "$item" &&
            problem "access token in $rel"
    done < <(find "$dir" -mindepth 1 -print0)

    for leaf in AppRun "$desktop_file" "usr/share/applications/$desktop_file" "$metainfo" "$icon.png" .DirIcon \
                "usr/share/icons/hicolor/256x256/apps/$icon.png" "usr/bin/$program" \
                usr/bin/assets/launcher/header-art.png usr/bin/assets/launcher/sponsor-heart.png \
                "$doc_dir/README.txt" "$license_dir/LICENSE.txt" "$license_dir/THIRD-PARTY-LICENSES.txt" \
                "$license_dir/SYSTEM-PACKAGES.txt" "$license_dir/ffmpeg/COPYING.LGPLv2.1" "$license_dir/ffmpeg/BUILD.txt" \
                "$license_dir/SMAA/LICENSE.txt" "$license_dir/shadPS4/LICENSE" "$license_dir/LibAtrac9/LICENSE"; do
        [[ -e $dir/$leaf || -L $dir/$leaf ]] || problem "missing: $leaf"
    done
    [[ -x $dir/AppRun && -x $dir/usr/bin/$program ]] || problem 'AppRun and the program must be executable'
    [[ ! -e $dir/usr/bin/assets/launcher/header-art-source.png ]] || problem 'header-art-source.png must not ship'
    if [[ -f $dir/AppRun ]]; then
        grep -q '@[A-Z_]*@' "$dir/AppRun" && problem 'AppRun has an unreplaced placeholder'
        grep -q "exec \"\$here/usr/bin/$program\" \"\$@\"" "$dir/AppRun" || problem 'AppRun does not start the program with its arguments'
    fi
    if [[ -f $dir/$desktop_file ]]; then
        grep -qx 'Exec=AppRun' "$dir/$desktop_file" && grep -qx 'Exec=AppRun --launcher' "$dir/$desktop_file" &&
            grep -qx "Icon=$icon" "$dir/$desktop_file" || problem "$desktop_file does not start AppRun with the $icon icon"
    fi
    if [[ -n $version && -f $dir/$metainfo ]]; then
        grep -q "<release version=\"$version\" date=\"[0-9]\{4\}-[0-9]\{2\}-[0-9]\{2\}\"/>" "$dir/$metainfo" ||
            problem "the metainfo has no release entry for $version"
        grep -q "^Version $version\$" "$dir/$doc_dir/README.txt" || problem "README.txt is not for version $version"
    fi
    # The launcher's About line carries the version the binary was built with.
    if [[ -n $version && -f $dir/usr/bin/$program ]] && ! grep -a -q -F "v$version   " "$dir/usr/bin/$program"; then
        problem "usr/bin/$program was not built as version $version"
    fi

    # FFmpeg must be the LGPL build of gpu/ffmpeg.txt: a GPL one cannot ship with this program.
    for lib in "$dir"/usr/lib/libav*.so* "$dir"/usr/lib/libsw*.so*; do
        [[ -f $lib && ! -L $lib ]] || continue
        grep -a -q -F 'LGPL version 2.1 or later' "$lib" || problem "not the LGPL FFmpeg: ${lib#"$dir"/}"
        grep -a -q -E -- '--enable-(gpl|nonfree)' "$lib" && problem "a GPL or non-free FFmpeg: ${lib#"$dir"/}"
    done

    if [[ -f $dir/usr/bin/$program ]]; then
        local header dynamic
        header=$(readelf -h -- "$dir/usr/bin/$program")
        grep -q 'Machine:.*X86-64' <<<"$header" || problem "usr/bin/$program is not x86_64"
        grep -q 'Type:.*EXEC' <<<"$header" || problem "usr/bin/$program must be a fixed-base EXEC (the guest heap stays below 1 TiB)"
        # DT_RPATH, not DT_RUNPATH: a host LD_LIBRARY_PATH (Steam's runtime) must not replace the bundled libraries.
        dynamic=$(readelf -d -- "$dir/usr/bin/$program")
        grep -q '(RPATH).*Library rpath: \[\$ORIGIN/\.\./lib\]$' <<<"$dynamic" ||
            problem "usr/bin/$program does not carry DT_RPATH \$ORIGIN/../lib alone"
        grep -q '(RUNPATH)' <<<"$dynamic" && problem "usr/bin/$program carries a DT_RUNPATH"
        # Every library resolves inside the AppDir or is one the host provides.
        while IFS= read -r line; do
            [[ $line == *'not found'* ]] && { problem "unresolved: $line"; continue; }
            [[ $line =~ ^[[:space:]]*([^[:space:]]+)\ =\>\ (/[^[:space:]]+) ]] || continue
            name=${BASH_REMATCH[1]}
            target=$(readlink -f -- "${BASH_REMATCH[2]}")
            is_host_lib "$name" && continue
            # The system's C++ library is preferred; AppRun adds usr/lib/compat only when it is older than the build's.
            if [[ $name =~ $compat_libs ]]; then
                [[ -f $dir/usr/lib/compat/$name ]] || problem "missing: usr/lib/compat/$name"
                continue
            fi
            [[ $target == "$dir"/usr/lib/* ]] || problem "$name resolves outside the AppDir: $target"
        done < <(env -u LD_LIBRARY_PATH ldd -- "$dir/usr/bin/$program" 2>&1 || true)
    fi
    for lib in "$dir"/usr/lib/*.so* "$dir"/usr/lib/compat/*.so*; do
        [[ -f $lib ]] || continue
        readelf -d -- "$lib" | grep -q '(RUNPATH)' && problem "${lib#"$dir"/} carries a DT_RUNPATH"
    done

    local group newest file highest
    for group in program libraries compat runtime; do
        highest=
        for file in "${files[@]}" ${appimage:+"$appimage"}; do
            case $group in
                program) [[ $file == "$dir/usr/bin/$program" ]] || continue ;;
                libraries) [[ $file == "$dir"/usr/lib/*.so* && $file != "$dir"/usr/lib/compat/* ]] || continue ;;
                compat) [[ $file == "$dir"/usr/lib/compat/*.so* ]] || continue ;;
                runtime) [[ -n $appimage && $file == "$appimage" ]] || continue ;;
            esac
            is_elf "$file" || continue
            newest=$(newest_glibc "$file") || { problem "unreadable ELF: ${file#"$dir"/}"; continue; }
            if [[ -n $newest ]] && newer_than "$newest" "$max_glibc"; then
                problem "${file#"$dir"/} needs glibc $newest, newer than the $max_glibc baseline (build on Ubuntu 20.04)"
            fi
            [[ -n $newest && ( -z $highest || $(newer_than "$newest" "$highest" && echo y) == y ) ]] && highest=$newest
        done
        [[ $group == runtime && -z $appimage ]] && continue
        echo "GLIBC_MAX|$group|${highest:-none}"
    done
    ((problems == 0)) || { echo "AppDir check failed: $problems problem(s)" >&2; return 1; }
    echo 'AppDir is clean'
}

# The runtime file system starts where the runtime ELF ends (its section header table is last).
appimage_offset() {
    local header shoff shnum shsize
    header=$(readelf -h -- "$1")
    shoff=$(awk -F: '/Start of section headers/ {gsub(/[^0-9]/, "", $2); print $2}' <<<"$header")
    shnum=$(awk -F: '/Number of section headers/ {gsub(/[^0-9]/, "", $2); print $2}' <<<"$header")
    shsize=$(awk -F: '/Size of section headers/ {gsub(/[^0-9]/, "", $2); print $2}' <<<"$header")
    echo $((shoff + shnum * shsize))
}

# The runtime at the front of the AppImage is the pinned one: equal bytes, apart from the sections appimagetool fills.
verify_embedded_runtime() { # appimage runtime
    local appimage=$1 runtime=$2 size offset work section start length masked=0
    size=$(stat -c %s -- "$runtime")
    offset=$(appimage_offset "$appimage")
    [[ $offset == "$size" ]] || { echo "the AppImage's file system starts at $offset, the pinned runtime is $size bytes" >&2; return 1; }
    work=$(mktemp -d)
    head -c "$size" -- "$appimage" > "$work/embedded"
    cp -- "$runtime" "$work/pinned"
    while read -r section start length; do
        for copy in embedded pinned; do
            dd if=/dev/zero of="$work/$copy" bs=1 seek=$((16#$start)) count=$((16#$length)) conv=notrunc status=none
        done
        masked=$((masked + 1))
    done < <(readelf -S -W -- "$work/pinned" | awk '{for (i = 1; i < NF - 3; i++) if ($i == ".digest_md5" || $i == ".upd_info" ||
        $i == ".sha256_sig" || $i == ".sig_key") print $i, $(i + 3), $(i + 4)}')
    local same=0
    ((masked > 0)) && cmp -s -- "$work/embedded" "$work/pinned" && same=1
    rm -rf -- "$work"
    ((same)) || { echo 'the runtime embedded in the AppImage is not the pinned runtime' >&2; return 1; }
    echo "Embedded runtime: the pinned runtime ($(sha256sum -- "$runtime" | cut -d' ' -f1)), $size bytes"
}

# A pinned packaging input: kept in build/appimage, imported from a sibling port's cache, or (appimagetool) downloaded.
pinned_input() { # file sha256 [url]
    local file=$1 sha=$2 url=${3:-}
    [[ -f $file ]] && echo "$sha  $file" | sha256sum -c --status - && return 0
    rm -f -- "$file"
    bash "$root/tools/import-appimage-inputs.sh" "$sha" "$file" >&2 && return 0
    [[ -n $url ]] || return 1
    curl -fL --retry 3 -o "$file.part" "$url" >&2
    echo "$sha  $file.part" | sha256sum -c --status - || { rm -f -- "$file.part"; return 1; }
    chmod 755 "$file.part"
    mv -f -- "$file.part" "$file"
}

pinned_runtime() { # file
    . "$root/patches/appimage-runtime/baseline.env"
    local imported=$1.imported
    if [[ -z ${SOTB_APPIMAGE_RUNTIME:-} ]] && pinned_input "$imported" "$RUNTIME_SHA256"; then
        SOTB_APPIMAGE_RUNTIME=$imported bash "$root/tools/build-appimage-runtime.sh" "$1" "$root/out/appimage-runtime"
    else
        bash "$root/tools/build-appimage-runtime.sh" "$1" "$root/out/appimage-runtime"
    fi
}

# Extracts an AppImage with its own runtime (--appimage-extract never starts AppRun) and checks everything in it.
verify_appimage() { # appimage [expected AppDir]
    local appimage version work status=0
    appimage=$(readlink -f -- "$1")
    [[ -f $appimage ]] || die "no AppImage at $1"
    version=$(sed -nE 's/^ShadowOfTheBeastPC-v([0-9]+\.[0-9]+\.[0-9]+)-x86_64\.AppImage(\.new)?$/\1/p' <<<"$(basename -- "$appimage")")
    [[ -n $version ]] || die "$(basename -- "$appimage") is not named $(appimage_name '<version>')"
    [[ $version == "$(project_version "$root/CMakeLists.txt")" ]] || die "$(basename -- "$appimage") is not the project version"
    [[ -x $appimage ]] || die "$appimage is not executable"
    [[ $(od -An -tx1 -j8 -N3 -- "$appimage" | tr -d ' \n') == 414902 ]] || die "$appimage is not a type 2 AppImage"
    bash "$root/tools/build-appimage-runtime.sh" --check "$appimage"
    local runtime=$root/build/appimage/runtime-sotb-x86_64
    pinned_runtime "$runtime" >/dev/null
    verify_embedded_runtime "$appimage" "$runtime"
    work=$(mktemp -d)
    (cd "$work" && "$appimage" --appimage-extract >/dev/null) || { rm -rf -- "$work"; die 'the AppImage could not be extracted'; }
    check_appdir "$work/squashfs-root" "$version" "$appimage" || status=1
    if ((status == 0)) && [[ -n ${2:-} ]]; then
        diff -r --no-dereference -- "$2" "$work/squashfs-root" >/dev/null || { echo 'the AppImage differs from the staged AppDir' >&2; status=1; }
    fi
    rm -rf -- "$work"
    ((status == 0)) || die "$(basename -- "$appimage") failed verification"
    local checksum=$appimage.sha256
    if [[ $appimage != *.new ]]; then
        [[ -f $checksum ]] || die "missing $(basename -- "$checksum")"
        [[ $(<"$checksum") =~ ^[0-9a-f]{64}\ \ $(basename -- "$appimage")$ ]] || die "$(basename -- "$checksum") is not '<sha256>  <file name>'"
        (cd "$(dirname -- "$appimage")" && sha256sum -c --status -- "$(basename -- "$checksum")") ||
            die "$(basename -- "$checksum") does not match the AppImage"
    fi
    echo "APPIMAGE_VERIFIED|$(basename -- "$appimage")|$version"
}

# Reads the AppImage's file system in place: neither the runtime nor the program is started.
extract_program() { # appimage out
    local appimage out=$2 work file
    appimage=$(readlink -f -- "$1")
    [[ -f $appimage ]] || die "no AppImage at $1"
    [[ $(od -An -tx1 -j8 -N3 -- "$appimage" | tr -d ' \n') == 414902 ]] || die "$appimage is not a type 2 AppImage"
    command -v unsquashfs >/dev/null || die 'unsquashfs is missing (squashfs-tools)'
    work=$(mktemp -d)
    unsquashfs -no-progress -offset "$(appimage_offset "$appimage")" -dest "$work/root" "$appimage" "usr/bin/$program" >/dev/null ||
        { rm -rf -- "$work"; die "unsquashfs could not read $(basename -- "$appimage")"; }
    file=$work/root/usr/bin/$program
    if [[ ! -f $file || -L $file ]] || ! is_elf "$file"; then rm -rf -- "$work"; die "the AppImage holds no usr/bin/$program program"; fi
    mkdir -p -- "$(dirname -- "$out")"
    cp -- "$file" "$out"
    rm -rf -- "$work"
    echo "PROGRAM_SHA256|$(sha256sum -- "$out" | cut -d' ' -f1)"
}

stage_licences() { # appdir ffmpeg-prefix owners-file
    local appdir=$1 ff_prefix=$2 owners=$3 licenses=$1/$license_dir component lib path package version_of
    install -Dm644 "$root/LICENSE" "$licenses/LICENSE.txt"
    install -Dm644 "$root/packaging/licenses/THIRD-PARTY-LICENSES.txt" "$licenses/THIRD-PARTY-LICENSES.txt"
    declare -A built_in=(
        [shadPS4]=gpu/shadps4/LICENSE [sirit]=gpu/third_party/sirit/LICENSE.txt
        [SPIRV-Headers]=gpu/third_party/sirit/externals/SPIRV-Headers/LICENSE [half]=gpu/third_party/half/LICENSE.txt
        [SMAA]=gpu/shadps4/video_core/host_shaders/smaa/LICENSE.txt [DejaVu]=gpu/third_party/fonts/LICENSE-DejaVu.txt
        [imgui]=gpu/third_party/imgui/LICENSE.txt [magic_enum]=gpu/third_party/magic_enum/LICENSE
        [miniz]=gpu/third_party/miniz/LICENSE [nanosvg]=launcher/third_party/nanosvg/LICENSE.txt
        [xbyak]=gpu/third_party/xbyak/COPYRIGHT [LibAtrac9]=third_party/LibAtrac9/LICENSE)
    for component in "${!built_in[@]}"; do
        [[ -f $root/${built_in[$component]} ]] || die "Missing licence file: ${built_in[$component]}"
        install -Dm644 "$root/${built_in[$component]}" "$licenses/$component/$(basename -- "${built_in[$component]}")"
    done
    for lib in LICENSE.md COPYING.LGPLv2.1 BUILD.txt; do
        install -Dm644 "$ff_prefix/share/licenses/ffmpeg/$lib" "$licenses/ffmpeg/$lib"
    done
    # The licence texts of the distribution packages the bundled libraries come from, or of the sources
    # packaging/linux/ci-deps.sh built them from (it leaves those texts in /usr/local/share/licenses/<component>).
    declare -A source_built=([libSDL3]=SDL3 [libfmt]=fmt [libZydis]=zydis [libZycore]=zydis [libxxhash]=xxhash
                             [libcrypto]=openssl [libssl]=openssl)
    local manifest=$licenses/SYSTEM-PACKAGES.txt
    {
        echo "Libraries bundled in the AppImage, with the Ubuntu packages or sources they come from and their licence texts."
        echo "Libraries and drivers the player's system provides (glibc, Vulkan, X11/Wayland, audio) are not bundled."
    } > "$manifest"
    while read -r lib path; do
        if [[ $(readlink -f -- "$path") == "$(readlink -f -- "$ff_prefix")"/lib/* ]]; then
            printf '\n%s (FFmpeg, built from gpu/ffmpeg.txt by tools/build-ffmpeg-linux.sh)\n  Licence text: ffmpeg/\n' "$lib" >> "$manifest"
            continue
        fi
        # dpkg knows a file by one name only, which on a merged-/usr system may be either of these.
        package=$(dpkg-query -S "$(readlink -f -- "$path")" "$path" 2>/dev/null | head -1 | cut -d: -f1 || true)
        if [[ -n $package ]]; then
            version_of=$(dpkg-query -W -f='${Version}' "$package")
            install -Dm644 "/usr/share/doc/$package/copyright" "$licenses/system/$package/copyright"
            printf '\n%s (%s %s)\n  Licence text: system/%s/copyright\n' "$lib" "$package" "$version_of" "$package" >> "$manifest"
        elif component=${source_built[${lib%%.so*}]:-} && [[ -n $component && $(readlink -f -- "$path") == /usr/local/lib/* ]]; then
            local texts=() text
            for text in /usr/local/share/licenses/"$component"/*; do [[ $text == *.stamp ]] || texts+=("$text"); done
            ((${#texts[@]})) && [[ -f ${texts[0]} ]] || die "No licence text in /usr/local/share/licenses/$component (run packaging/linux/ci-deps.sh)"
            mkdir -p "$licenses/system/$component"
            install -m644 "${texts[@]}" "$licenses/system/$component/"
            printf '\n%s (%s, built from source by packaging/linux/ci-deps.sh)\n  Licence text: system/%s/\n' \
                "$lib" "$component" "$component" >> "$manifest"
        else
            die "No package owns $path: add its licence handling to tools/package-linux.sh"
        fi
    done < <(sort -u "$owners")
}

main() {
    local skip_build=0 verify_dir= verify_file= out_dir=
    while (($#)); do
        case $1 in
            --skip-build) skip_build=1 ;;
            --out-dir) out_dir=${2:?--out-dir needs a folder}; shift ;;
            --verify) verify_dir=$(cd -- "${2:?--verify needs a folder}" && pwd) || exit 2; shift ;;
            --verify-appimage) verify_file=${2:?--verify-appimage needs a file}; shift ;;
            --extract-program)
                for tool in readelf od sha256sum; do command -v "$tool" >/dev/null || die "Missing $tool"; done
                extract_program "${2:?--extract-program needs an AppImage}" "${3:?--extract-program needs an output file}"
                return ;;
            -h|--help) sed -n '2,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
            *) echo "Unknown option: $1" >&2; exit 2 ;;
        esac
        shift
    done
    cd "$root"
    local version
    version=$(project_version CMakeLists.txt)
    [[ -n $version ]] || die 'project VERSION not found in CMakeLists.txt'
    if [[ -n $verify_dir ]]; then
        check_appdir "$verify_dir" "$version"
        return
    fi
    for tool in readelf objdump objcopy strip patchelf ldd sha256sum od perl; do
        command -v "$tool" >/dev/null || die "Missing $tool (packaging/linux/ci-deps.sh)"
    done
    if [[ -n $verify_file ]]; then
        verify_appimage "$verify_file"
        return
    fi

    [[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || die 'Packaging needs Linux x86_64 (Windows: tools\package.ps1).'
    for tool in curl dpkg-query desktop-file-validate install; do
        command -v "$tool" >/dev/null || die "Missing $tool (packaging/linux/ci-deps.sh)"
    done
    local host_glibc
    host_glibc=$(getconf GNU_LIBC_VERSION | cut -d' ' -f2)
    check_build_host "$host_glibc"
    local release_date=${SOTB_RELEASE_DATE:-$(date -u +%Y-%m-%d)}
    [[ $release_date =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]] || die "SOTB_RELEASE_DATE is not YYYY-MM-DD: $release_date"

    local name dist work appdir symbols build_dir loader
    name=$(appimage_name "$version")
    name=${name%.AppImage}
    dist=${out_dir:-$root/dist}
    mkdir -p -- "$dist" && dist=$(cd -- "$dist" && pwd)
    work=$root/build/appimage
    appdir=$work/AppDir
    symbols=$dist/$name-symbols
    build_dir=$root/build/cmake/linux
    loader=$build_dir/bin/$program

    step "Shadow of the Beast PC $version - Linux AppImage (local only, glibc $host_glibc)"
    ((skip_build)) || bash "$root/tools/build.sh"
    [[ -x $loader ]] || die "Missing $loader (run without --skip-build)"
    grep -q "^#define SOTB_VERSION_STRING \"$version\"$" "$build_dir/generated/sotb_version.h" ||
        die "The Linux build is not version $version (run without --skip-build)"
    local ff_prefix
    ff_prefix=$(sed -n 's/^SOTB_FFMPEG_PREFIX:PATH=//p' "$build_dir/CMakeCache.txt")
    [[ -d $ff_prefix/lib ]] || die "The FFmpeg the build used is missing: $ff_prefix"
    bash "$root/tools/build-ffmpeg-linux.sh" --check "$ff_prefix" "$ff_prefix.work" | grep -x 'FFMPEG|current' >/dev/null ||
        die "$ff_prefix is not the current pinned FFmpeg build (run without --skip-build)"

    rm -rf -- "$appdir" "$symbols" "$dist/$name.AppImage" "$dist/$name.AppImage.sha256" "$dist/$name.AppImage.new"
    mkdir -p "$symbols" "$appdir/usr/bin/assets/launcher" "$appdir/usr/lib/compat" "$appdir/usr/share/applications" \
             "$appdir/usr/share/metainfo" "$appdir/usr/share/icons/hicolor/256x256/apps" "$appdir/$doc_dir"

    step "Staging $program without debug information"
    objcopy --only-keep-debug "$loader" "$symbols/$program.debug"
    objcopy --strip-debug --add-gnu-debuglink="$symbols/$program.debug" "$loader" "$appdir/usr/bin/$program"
    chmod 755 "$appdir/usr/bin/$program"
    install -m644 assets/launcher/header-art.png assets/launcher/sponsor-heart.png "$appdir/usr/bin/assets/launcher/"

    step 'Staging the libraries it loads'
    local owners=$work/owners.txt
    : > "$owners"
    declare -A seen=()
    local queue=("$loader") current line lib file real
    while ((${#queue[@]})); do
        current=${queue[0]}
        queue=("${queue[@]:1}")
        while IFS= read -r line; do
            [[ $line != *'not found'* ]] || die "unresolved library of $(basename -- "$current"): $line"
            [[ $line =~ ^[[:space:]]*([^[:space:]]+)\ =\>\ (/[^[:space:]]+) ]] || continue
            file=${BASH_REMATCH[1]}
            lib=${BASH_REMATCH[2]}
            [[ -z ${seen[$file]:-} ]] || continue
            seen[$file]=1
            is_host_lib "$file" && continue
            real=$(readlink -f -- "$lib")
            if [[ $file =~ ^lib(av|sw)[a-z]+\.so && $real != "$ff_prefix"/lib/* ]]; then
                die "$file comes from $real, not from the pinned FFmpeg in $ff_prefix"
            fi
            echo "$file $lib" >> "$owners"
            if [[ $file =~ $compat_libs ]]; then
                install -m644 "$real" "$appdir/usr/lib/compat/$file"
                echo "  [compat] $file <= $real"
            else
                install -m644 "$real" "$appdir/usr/lib/$file"
                echo "  [lib]    $file <= $real"
                queue+=("$real")
            fi
        done < <(env -u LD_LIBRARY_PATH ldd -- "$current")
    done
    for lib in "$appdir"/usr/lib/*.so* "$appdir"/usr/lib/compat/*.so*; do
        [[ -f $lib ]] || continue
        strip --strip-unneeded "$lib"
        patchelf --remove-rpath "$lib"
    done
    for lib in "$appdir"/usr/lib/*.so*; do
        [[ -f $lib ]] && patchelf --force-rpath --set-rpath '$ORIGIN' "$lib"
    done
    # DT_RPATH (not RUNPATH) is inherited by the libraries' own dependencies and wins over LD_LIBRARY_PATH. patchelf
    # keeps the build's RUNPATH text in the string table, so it is blanked: it names the build folder.
    local build_rpath
    build_rpath=$(patchelf --print-rpath "$appdir/usr/bin/$program")
    patchelf --remove-rpath "$appdir/usr/bin/$program"
    patchelf --force-rpath --set-rpath '$ORIGIN/../lib' "$appdir/usr/bin/$program"
    if [[ -n $build_rpath ]]; then
        LC_ALL=C perl -0777 -pi -e 'BEGIN { $p = shift } s/\0\Q$p\E\0/"\0" x (length($p) + 2)/ge' -- "$build_rpath" "$appdir/usr/bin/$program"
    fi
    echo "Bundled: $(cd "$appdir/usr/lib" && ls -- *.so* | tr '\n' ' ')"
    echo "Fallback: $(cd "$appdir/usr/lib/compat" && ls -- *.so* 2>/dev/null | tr '\n' ' ')"

    step 'Staging AppRun, desktop entry, metainfo, icon, README and licences'
    local glibcxx_need gcc_need
    glibcxx_need=$(objdump -T "$appdir/usr/bin/$program" "$appdir"/usr/lib/*.so* | grep -o 'GLIBCXX_3\.4\.[0-9]*' |
                   sed 's/.*\.//' | sort -n | tail -1 || true)
    gcc_need=$(objdump -T "$appdir/usr/bin/$program" "$appdir"/usr/lib/*.so* | grep -o 'GCC_[0-9][0-9.]*' |
               sed 's/^GCC_//; s/\..*//' | sort -n | tail -1 || true)
    sed "s/@GLIBCXX_NEED@/${glibcxx_need:-0}/g; s/@GCC_NEED@/${gcc_need:-0}/g" packaging/linux/AppRun > "$appdir/AppRun"
    chmod 755 "$appdir/AppRun"
    install -m644 "packaging/linux/$desktop_file" "$appdir/$desktop_file"
    install -m644 "packaging/linux/$desktop_file" "$appdir/usr/share/applications/$desktop_file"
    desktop-file-validate "$appdir/$desktop_file" || die "$desktop_file does not validate"
    sed "s/@VERSION@/$version/; s/@DATE@/$release_date/" "packaging/linux/$app_id.appdata.xml" > "$appdir/$metainfo"
    if command -v appstreamcli >/dev/null; then
        appstreamcli validate --no-net "$appdir/$metainfo" || die "$metainfo does not validate"
    fi
    install -m644 packaging/icons/app-icon-256.png "$appdir/$icon.png"
    install -m644 packaging/icons/app-icon-256.png "$appdir/usr/share/icons/hicolor/256x256/apps/$icon.png"
    ln -s "$icon.png" "$appdir/.DirIcon"
    sed "s/@VERSION@/$version/" packaging/README-linux.txt > "$appdir/$doc_dir/README.txt"
    stage_licences "$appdir" "$ff_prefix" "$owners"

    step 'Checking the AppDir'
    check_appdir "$appdir" "$version"

    step 'Writing the AppImage with the pinned appimagetool and runtime'
    local tool=$work/appimagetool-$appimagetool_version-x86_64.AppImage runtime=$work/runtime-sotb-x86_64
    pinned_input "$tool" "$appimagetool_sha256" \
        "https://github.com/AppImage/appimagetool/releases/download/$appimagetool_version/appimagetool-x86_64.AppImage" ||
        die "appimagetool $appimagetool_version with SHA-256 $appimagetool_sha256 is not available"
    # Without --runtime-file appimagetool would download the unpinned "continuous" runtime.
    pinned_runtime "$runtime"
    local image=$dist/$name.AppImage.new
    ARCH=x86_64 "$tool" --appimage-extract-and-run --no-appstream --runtime-file "$runtime" "$appdir" "$image" \
        > "$work/appimagetool.log" 2>&1 || { cat "$work/appimagetool.log" >&2; die 'appimagetool failed'; }
    chmod 755 "$image"

    step 'Checking the written AppImage (extracted, never started)'
    verify_appimage "$image" "$appdir"
    mv -f -- "$image" "$dist/$name.AppImage"
    (cd "$dist" && sha256sum -- "$name.AppImage" > "$name.AppImage.sha256" && sha256sum -c --status -- "$name.AppImage.sha256")

    printf '\nAPPIMAGE READY (nothing was published)\n'
    printf '  AppImage: %s (%s)\n' "$dist/$name.AppImage" "$(du -h -- "$dist/$name.AppImage" | cut -f1)"
    printf '  SHA-256:  %s\n' "$(cut -d' ' -f1 "$dist/$name.AppImage.sha256")"
    printf '  Symbols:  %s (not in the package)\n' "$symbols/$program.debug"
}

[[ ${BASH_SOURCE[0]} != "$0" ]] || main "$@"
