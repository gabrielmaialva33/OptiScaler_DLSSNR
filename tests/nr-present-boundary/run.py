#!/usr/bin/env python3
"""Exercise DlssNr::PresentBoundary: the present pass under the game's own DLSS-G, and the source guards that keep
both routes deciding through it."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]

# Source guards. The decision is only worth testing if production makes it here, so both call sites must go through
# the header, and the inline conditions they replaced must not come back beside it.
source = (root / 'retoque/shaders/dlssnr/DlssNr_Dx12.cpp').read_text(encoding='utf-8-sig')
run_present = source[source.index('void RunPresentPass('):]
run_present = run_present[:run_present.index('\n}\n')]
failures = []
if 'PresentBoundary::Decide(' not in run_present:
    failures.append('RunPresentPass no longer decides through PresentBoundary::Decide')
if 'PresentVerdict::Declined' not in run_present:
    failures.append('RunPresentPass no longer handles the Declined verdict')
if 'UpscaledDefersToPresent(' not in source or 'UpscaledCapturesForPresent(' not in source:
    failures.append('the upscaled route no longer asks PresentBoundary whether to defer and capture')
if re.search(r'hookMethod\s*==\s*0\s*&&\s*(g_temporalValid\.load\(\)\s*&&\s*)?PresentHookLive\(\)', source):
    failures.append('an inline Auto deferral (hookMethod == 0 && PresentHookLive()) is back beside the header')
if failures:
    for failure in failures:
        print(f'FAIL: {failure}')
    sys.exit(1)
print('PASS: both routes decide through DlssNr_PresentBoundary.h')

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-present-boundary-') as directory:
    binary = Path(directory) / 'cases'
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(root / 'retoque'),
                    str(here / 'cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
