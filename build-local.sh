#!/usr/bin/env bash
# Local OptiScaler_DLSSNR build with msvc-wine (MSVC 14.44 + Windows SDK under Wine).
# Usage: ./build-local.sh [Release|Debug]   -> outputs in x64/<Config>/ and copies OptiScaler.dll + nvngx.dll_dlssnr.dll to out/
set -euo pipefail
CONFIG="${1:-Release}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
MSVC_BIN="${MSVC_BIN:-$HOME/.local/opt/msvc/bin/x64}"
export WINEPREFIX="${WINEPREFIX:-$HOME/.local/opt/msvc-wineprefix}"
export WINEDEBUG="${WINEDEBUG:--all}"

# What the VS pre-build PowerShell step would generate:
date_str="$(date '+%Y%m%d_%H%M%S')"
commit="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
printf '#define VER_BUILD_DATE "%s"\n'   "$date_str" > "$ROOT/OptiScaler/resource_build_date.h"
printf '#define VER_BUILD_COMMIT "%s"\n' "$commit"   > "$ROOT/OptiScaler/resource_build_commit.h"

cd "$ROOT"
"$MSVC_BIN/msbuild" OptiScaler.sln /nologo /v:minimal /p:CL_MPCount=16 \
  /p:Configuration="$CONFIG" /p:Platform=x64 \
  /p:PreBuildEventUseInBuild=false /p:PostBuildEventUseInBuild=false \
  "${@:2}"

mkdir -p "$ROOT/x64/out"
for f in OptiScaler.dll nvngx.dll_dlssnr.dll; do
  src="$(find "$ROOT/x64/$CONFIG" -maxdepth 2 -name "$f" | head -1 || true)"
  [ -n "$src" ] && cp -f "$src" "$ROOT/x64/out/$f" && echo "x64/out/$f  <- $src"
done
echo "done: $(strings "$ROOT/x64/out/OptiScaler.dll" 2>/dev/null | grep -oE '[0-9a-f]{7}\) \(2026[0-9_]+\)' | head -1)"
