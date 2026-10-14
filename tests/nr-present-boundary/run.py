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
# Step 0's probe (DlssNr_SlBoundaryProbe.cpp) only watches. It installs nothing outside [DlssNr] Enabled with
# HookMethod=2, every note returns at once when it is not installed, and each call site is there exactly once.
probe = (root / 'retoque/dlssnr/DlssNr_SlBoundaryProbe.cpp').read_text(encoding='utf-8-sig')
install = probe[probe.index('void DlssNr::SlBoundaryProbe::Install('):]
install = install[:install.index('\n}\n')]
gate = 'if (interposer == nullptr || !cfg.DlssNrEnabled.value_or_default() || cfg.DlssNrHookMethod.value_or_default() != 2)'
if gate not in install or install.index(gate) > install.index('DetourTransactionBegin()'):
    failures.append('the probe can install without [DlssNr] Enabled and HookMethod=2')
for note in ('NoteWrappedPresent(', 'NoteDlssgEvaluate(', 'NoteExecute('):
    body = probe[probe.index(f'void DlssNr::SlBoundaryProbe::{note}'):]
    first = body[body.index('{') + 1:].lstrip()
    if not first.startswith('if (!g_installed.load()'):
        failures.append(f'{note} does work before checking that the probe is installed')
for hook in ('hkPresent(', 'hkPresent1('):
    # The definition, not the forward declaration: the signature followed by a body.
    definition = re.search(r'HRESULT STDMETHODCALLTYPE ' + re.escape(hook) + r'[^;{]*\)\n\{', probe)
    body = probe[definition.end():] if definition else ''
    body = body[:body.index('\n}\n')] if body else ''
    if not body.lstrip().startswith('if (!g_installed.load())'):
        failures.append(f'{hook} does work before checking that the probe is installed')
sites = {
    'OptiScaler/hooks/Streamline_Hooks.cpp': ('DlssNr::SlBoundaryProbe::Install(slInterposer);',
                                              'DlssNr::SlBoundaryProbe::Uninstall();'),
    'OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp': ('DlssNr::SlBoundaryProbe::NoteDlssgEvaluate(InCmdList, InParameters);',),
    'OptiScaler/resource_tracking/ResTrack_dx12.cpp': (
        'DlssNr::SlBoundaryProbe::NoteExecute(This, NumCommandLists, ppCommandLists);',),
    'retoque/shaders/dlssnr/DlssNr_Dx12.cpp': ('DlssNr::SlBoundaryProbe::NoteWrappedPresent(queue, fgHook);',),
}
for path, calls in sites.items():
    text = (root / path).read_text(encoding='utf-8-sig')
    for call in calls:
        if text.count(call) != 1:
            failures.append(f'{path}: expected one {call}')

# Step 1: the boundary runs the pass before forwarding, on the game's native queue, and the entries below it stand down
# while it is live; at the boundary PresentSync=auto does not wait.
enter = probe[probe.index('void Enter(IUnknown* swapchain, UINT flags)'):]
enter = enter[:enter.index('\n}\n')]
if enter.count('DlssNr::RunPresentPass(sc3, queue, false, true);') != 1:
    failures.append('the boundary no longer runs the present pass (RunPresentPass(..., boundary=true)) in Enter')
if 'g_gameQueueNative.load()' not in enter:
    failures.append('the boundary pass is not submitted on the game\'s native queue')
stand_down = 'else if (BoundaryLive())\n    {\n        return;\n    }'
if stand_down not in run_present or run_present.index(stand_down) > run_present.index('++g_presentFlip;'):
    failures.append('the entries below the boundary do not stand down, uncounted, while it is live')
if 'syncInputs.boundary = boundary;' not in run_present:
    failures.append('PresentSync is not told the pass ran at the boundary')

if failures:
    for failure in failures:
        print(f'FAIL: {failure}')
    sys.exit(1)
print('PASS: both routes decide through DlssNr_PresentBoundary.h; the boundary installs only with HookMethod=2, '
      'runs the pass before forwarding on the game\'s native queue, and the entries below stand down while it is live')

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-present-boundary-') as directory:
    binary = Path(directory) / 'cases'
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(root / 'retoque'),
                    str(here / 'cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
