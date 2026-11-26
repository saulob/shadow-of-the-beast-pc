#!/usr/bin/env bash
# Copies a reviewed AppImage packaging input (the patched runtime or appimagetool) to TARGET from a sibling port's WSL
# packaging cache that pins the same file. Only a file whose SHA-256 is SHA256 is copied; nothing is downloaded.
#
#   tools/import-appimage-inputs.sh SHA256 TARGET
set -euo pipefail
sha256=${1:?usage: tools/import-appimage-inputs.sh SHA256 TARGET}
target=${2:?usage: tools/import-appimage-inputs.sh SHA256 TARGET}
[[ $sha256 =~ ^[0-9a-f]{64}$ ]] || { echo "ERROR: not a SHA-256: $sha256" >&2; exit 2; }
cache=${XDG_CACHE_HOME:-$HOME/.cache}
shopt -s nullglob
for candidate in "$cache"/{alienation-pc,god-of-war-sons-of-sparta-pc,13-sentinels-aegis-rim-pc,dragons-crown-pro-pc,odin-sphere-leifthrasir-pc}-build/build/appimage/{runtime-*x86_64,appimagetool-*-x86_64.AppImage}; do
    [[ -f $candidate && ! -L $candidate && $(sha256sum -- "$candidate" | cut -d' ' -f1) == "$sha256" ]] || continue
    mkdir -p -- "$(dirname -- "$target")"
    cp -- "$candidate" "$target.part"
    [[ $(sha256sum -- "$target.part" | cut -d' ' -f1) == "$sha256" ]] || { rm -f -- "$target.part"; continue; }
    chmod 755 -- "$target.part"
    mv -f -- "$target.part" "$target"
    echo "Imported $(basename -- "$target") (sha256 $sha256) from $candidate"
    exit 0
done
echo "No WSL packaging cache holds the file with SHA-256 $sha256" >&2
exit 1
