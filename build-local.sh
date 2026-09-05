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

# Under Wine, MSBuild stalls intermittently: every cl.exe sits at 0 CPU while wineserver spins
# on a core, the log stops growing, and nothing ever resumes. It hits any translation unit, both
# projects, and every parallelism setting (CL_MPCount 16/4/1 and /MP off were all tried and all
# stalled). Root cause is unknown -- it looks like a wineserver wait that never completes. What
# has held every single time is the recovery: kill msbuild, reset the prefix's wineserver, run
# again, and the incremental build resumes where it stopped. So that recovery lives here, as a
# watchdog with a bounded retry, instead of in someone's memory.
#
# The forwarder is built in its own msbuild process first. Seven of eight forwarder stalls were at
# the in-process transition from OptiScaler.vcxproj to dlssnr_forwarder.vcxproj; built alone it has
# never stalled.
common=(/nologo /v:minimal /p:CL_MPCount=16 /p:Configuration="$CONFIG" /p:Platform=x64
        /p:PreBuildEventUseInBuild=false /p:PostBreakEventUseInBuild=false
        "/p:SolutionDir=$(printf 'Z:%s\\' "$ROOT" | sed 's|/|\\|g')")
common=("${common[@]/PostBreakEventUseInBuild/PostBuildEventUseInBuild}")

# cpu_ticks <pid>: user+system jiffies, for measuring deltas. ps %CPU is an average since start
# and reads low for a process that worked and then stalled; deltas do not lie.
cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }

# stalled: true when, over one sample, no cl.exe used any CPU while wineserver did, and the log
# did not grow. Matches the bracket-quoted pattern so it never matches this script's own shell.
stalled() {
  local log=$1 s1 s2 t=0 a b p
  s1=$(stat -c%s "$log" 2>/dev/null || echo 0)
  local cls; cls=$(pgrep -f '[H]ostX64.x64.CL\.exe' 2>/dev/null)
  [ -z "$cls" ] && return 1                       # nothing compiling: linking, or done
  for p in $cls; do a=$(cpu_ticks "$p"); sleep 1; b=$(cpu_ticks "$p"); t=$((t + b - a)); done
  s2=$(stat -c%s "$log" 2>/dev/null || echo 0)
  [ "$t" -eq 0 ] && [ "$s1" = "$s2" ]
}

# run_msbuild <label> <log> <msbuild args...>: runs msbuild under the watchdog. Three consecutive
# stalled samples 30 s apart (about 90 s with nothing happening) count as a hang; the attempt is
# torn down, the wineserver is reset, and the next attempt resumes incrementally. Up to 5 attempts.
run_msbuild() {
  local label=$1 log=$2; shift 2
  local attempt dead pid rc
  for attempt in 1 2 3 4 5; do
    : > "$log"
    "$MSVC_BIN/msbuild" "$@" >> "$log" 2>&1 &
    pid=$!
    dead=0
    while kill -0 "$pid" 2>/dev/null; do
      sleep 30
      if stalled "$log"; then
        dead=$((dead + 1))
        echo "[$label] attempt $attempt: stalled sample $dead/3 (last: $(tail -n1 "$log" | tr -d ' ' | cut -c1-60))" >&2
      else
        dead=0
      fi
      if [ "$dead" -ge 3 ]; then
        echo "[$label] attempt $attempt: hung; resetting wineserver and retrying" >&2
        kill -TERM "$pid" 2>/dev/null
        for p in $(pgrep -f '[M]SBuild\.exe') $(pgrep -f '[H]ostX64.x64.CL\.exe'); do kill -TERM "$p" 2>/dev/null; done
        sleep 2; wineserver -k 2>/dev/null; sleep 2
        for p in $(pgrep -x winedevice.exe) $(pgrep -x services.exe) $(pgrep -x plugplay.exe); do kill -KILL "$p" 2>/dev/null; done
        break
      fi
    done
    if [ "$dead" -lt 3 ]; then
      wait "$pid"; rc=$?
      cat "$log"
      return $rc
    fi
  done
  echo "[$label] gave up after 5 attempts" >&2
  cat "$log"
  return 1
}

run_msbuild forwarder "$ROOT/x64/build-forwarder.log" \
  OptiScaler/dlssnr/forwarder/dlssnr_forwarder.vcxproj "${common[@]}" "${@:2}"
run_msbuild optiscaler "$ROOT/x64/build-optiscaler.log" \
  OptiScaler/OptiScaler.vcxproj "${common[@]}" "${@:2}"

mkdir -p "$ROOT/x64/out"
for f in OptiScaler.dll nvngx.dll_dlssnr.dll; do
  src="$(find "$ROOT/x64/$CONFIG" -maxdepth 2 -name "$f" | head -1 || true)"
  [ -n "$src" ] && cp -f "$src" "$ROOT/x64/out/$f" && echo "x64/out/$f  <- $src"
done
echo "done: $(strings "$ROOT/x64/out/OptiScaler.dll" 2>/dev/null | grep -oE '[0-9a-f]{7}\) \(2026[0-9_]+\)' | head -1)"
