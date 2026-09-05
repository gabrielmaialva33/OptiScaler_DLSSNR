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

# Two msbuild processes, not one solution build. Under Wine the solution build stalls at the
# transition from OptiScaler.vcxproj to dlssnr_forwarder.vcxproj (seven of eight stalls observed
# were exactly there; the forwarder built fine on its own every time). Building the forwarder in
# its own process first, then the DLL, removes the in-process project transition entirely.
# The forwarder never defined MultiProcessorCompilation; the DLL does, and /MP's parent/child
# cl.exe pair coordinates through wineserver, so it is switched off for the DLL via an imported
# ItemDefinitionGroup (a global /p: cannot override item metadata). Serial, but it finishes.
common=(/nologo /v:minimal /p:Configuration="$CONFIG" /p:Platform=x64
        /p:PreBuildEventUseInBuild=false /p:PostBuildEventUseInBuild=false
        "/p:SolutionDir=$(printf 'Z:%s\\' "$ROOT" | sed 's|/|\\|g')")
nomp="$ROOT/x64/no-mp.targets"
mkdir -p "$ROOT/x64"
cat > "$nomp" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemDefinitionGroup><ClCompile><MultiProcessorCompilation>false</MultiProcessorCompilation></ClCompile></ItemDefinitionGroup>
</Project>
XML
nomp_win="$(printf 'Z:%s' "$nomp" | sed 's|/|\\|g')"

"$MSVC_BIN/msbuild" OptiScaler/dlssnr/forwarder/dlssnr_forwarder.vcxproj "${common[@]}" "${@:2}"
"$MSVC_BIN/msbuild" OptiScaler/OptiScaler.vcxproj "${common[@]}" \
  "/p:ForceImportBeforeCppTargets=$nomp_win" "${@:2}"

mkdir -p "$ROOT/x64/out"
for f in OptiScaler.dll nvngx.dll_dlssnr.dll; do
  src="$(find "$ROOT/x64/$CONFIG" -maxdepth 2 -name "$f" | head -1 || true)"
  [ -n "$src" ] && cp -f "$src" "$ROOT/x64/out/$f" && echo "x64/out/$f  <- $src"
done
echo "done: $(strings "$ROOT/x64/out/OptiScaler.dll" 2>/dev/null | grep -oE '[0-9a-f]{7}\) \(2026[0-9_]+\)' | head -1)"
