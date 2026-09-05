#!/usr/bin/env python3
"""Test the production lifetime model; no GPU, Wine or game side effects."""
from pathlib import Path
import subprocess

here = Path(__file__).resolve().parent
root = here.parents[1]
out = here / 'artifacts'
out.mkdir(exist_ok=True)
subprocess.run(['g++', '-std=c++20', '-g', '-O1', '-pthread', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I' + str(root),
                str(here / 'model.cpp'), '-o', str(out / 'model')], check=True)
subprocess.run([str(out / 'model')], check=True)

# Boundary guard: both original queue-call paths must notify, always after the call.
source = (root / 'OptiScaler/resource_tracking/ResTrack_dx12.cpp').read_text()
body = source.split('void ResTrack_Dx12::hkExecuteCommandLists(', 1)[1].split('#pragma region Heap hooks', 1)[0]
assert body.count('o_ExecuteCommandLists(This, NumCommandLists, ppCommandLists);') == 2
assert body.count('nrSubmission.Submitted();') == 2
for tail in body.split('o_ExecuteCommandLists(This, NumCommandLists, ppCommandLists);')[1:]:
    assert tail.lstrip().startswith('nrSubmission.Submitted();')
assert body.index('Submission::Batch') < body.index('o_ExecuteCommandLists(')
print('PASS: both queue-hook paths reserve before execution and notify after original call')
