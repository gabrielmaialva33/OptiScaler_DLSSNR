#!/usr/bin/env python3
"""Session guard (docs/dlssnr/design/session-guard.md): the production decision header compiled under ASan/UBSan and
driven with a fake environment, plus source guards on the call sites and the config round trip."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
failures = []


def read(relative):
    return (root / relative).read_text()


def require(condition, message):
    if not condition:
        failures.append(message)


# --- Source guards -------------------------------------------------------------------------------------------

dllmain = read('OptiScaler/dllmain.cpp')
dx12 = read('retoque/shaders/dlssnr/DlssNr_Dx12.cpp')
vk = read('retoque/dlssnr/DlssNrFeature_Vk.cpp')
menu = read('retoque/dlssnr/DlssNr_Menu.cpp')
session = read('retoque/dlssnr/DlssNr_Session.cpp')
ini = read('OptiScaler.ini')

require('DlssNr::SessionGuardBegin();' in dllmain, 'dllmain.cpp never starts the guard')
detach = dllmain[dllmain.index('case DLL_PROCESS_DETACH:'):]
off_at = detach.find('DlssNr::SessionGuardOffAtExit();')
return_at = detach.find('if (lpReserved != nullptr)')
require(0 <= off_at < return_at, 'the guard must be told of an orderly exit before the lpReserved early break')
require('SessionGuardEvaluated' in dx12 and 'SessionGuardEvaluated' in vk, 'an NR evaluate does not mark the session')
require(dx12.count('DlssNr::SessionGuardOff();') >= 4, 'the D3D12 off paths and the fault latch do not clear the marker')
require('SessionGuardOff();' in vk, 'the Vulkan off path does not clear the marker')
require('SessionGuardEnableAnyway' in menu, 'the menu has no Enable anyway')
# The disable is session-only: through the volatile setter, never a plain assignment that SaveIni would write.
require('DlssNrEnabled.set_volatile_value(false)' in session, 'the guard must disable through set_volatile_value')
require(not re.search(r'DlssNrEnabled\s*=\s*false', session), 'the guard must never assign DlssNrEnabled = false')
require(re.search(r'^SessionGuard=', ini[ini.index('[DlssNr]'):], re.M) is not None, 'SessionGuard missing from [DlssNr]')

# --- The policy table ----------------------------------------------------------------------------------------

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-session-guard-') as directory:
    binary = Path(directory) / 'cases'
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(root / 'retoque'), str(here / 'cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

if failures:
    for failure in failures:
        print('FAIL:', failure)
    raise SystemExit(1)

print('PASS: session guard source guards')
