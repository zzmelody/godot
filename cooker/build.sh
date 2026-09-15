#!/usr/bin/env bash
# /*<<----- VEYA_COOKER: reproducible macOS build entry, never updates the pinned source. */
set -euo pipefail

cooker_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cooker_arch="$(uname -m)"
cooker_jobs="$(sysctl -n hw.logicalcpu 2>/dev/null || echo 8)"
cooker_lto="none"
cooker_compiledb="no"
cooker_scons="${VEYA_SCONS:-scons}"
cooker_luau_source="../luau"

while (($#)); do
    case "$1" in
        --arch)
            cooker_arch="$2"
            shift 2
            ;;
        --jobs|-j)
            cooker_jobs="$2"
            shift 2
            ;;
        --lto)
            cooker_lto="$2"
            shift 2
            ;;
        --compiledb)
            cooker_compiledb="yes"
            shift
            ;;
        --luau-source)
            cooker_luau_source="$2"
            shift 2
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "cooker/build.sh requires macOS." >&2
    exit 2
fi
if [[ "$cooker_arch" != "arm64" && "$cooker_arch" != "x86_64" ]]; then
    echo "Unsupported macOS architecture: $cooker_arch" >&2
    exit 2
fi
if ! [[ "$cooker_jobs" =~ ^[1-9][0-9]*$ ]]; then
    echo "--jobs must be a positive integer." >&2
    exit 2
fi
if [[ "$cooker_lto" != "none" && "$cooker_lto" != "full" && "$cooker_lto" != "auto" ]]; then
    echo "--lto must be none, full or auto." >&2
    exit 2
fi

cd "$cooker_root"
"$cooker_scons" \
    platform=macos \
    profile=cooker/profile.py \
    "arch=$cooker_arch" \
    metal=no \
    "lto=$cooker_lto" \
    "compiledb=$cooker_compiledb" \
    "-j$cooker_jobs" \
    "veya_luau_source=$cooker_luau_source"
printf '%s\n' "$cooker_root/bin/veya_cooke"
# /*>>----- VEYA_COOKER */
