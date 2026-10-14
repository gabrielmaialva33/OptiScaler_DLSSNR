#!/usr/bin/env bash
# Stub: runs the test suites via tests/run_all.py.
exec python3 "$(dirname "${BASH_SOURCE[0]}")/tests/run_all.py" "$@"
