#!/usr/bin/env bash
# Serial rendering regressions against host policy, real D3D12, NR and DLSS-G.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
host_only=false
build=true
for argument in "$@"; do
  case "$argument" in
    --host-only) host_only=true ;;
    --no-build) build=false ;;
    -h|--help)
      echo 'Usage: tools/test-rendering.sh [--host-only] [--no-build]'
      echo 'Runs host suites, Release build, D3D12 input checks, moving NR and real DLSS-G serially.'
      echo '--host-only runs the host tier only; --no-build uses the existing x64/out DLL.'
      exit 0 ;;
    *) echo "Unknown argument: $argument" >&2; exit 2 ;;
  esac
done
cd "$ROOT"
export PYTHONUNBUFFERED=1
python3 tests/run_all.py --tier host --jobs 4
if "$host_only"; then
  exit 0
fi
if "$build"; then
  ./build-local.sh Release
fi
python3 tests/fg-inputs-d3d12/run.py
python3 tests/dlssnr-loopback/run.py --runtime proton --moving-scene
exec python3 tests/fg-provider-d3d12/run.py --strict
