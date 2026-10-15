#!/usr/bin/env python3
"""Session guard (docs/dlssnr/design/session-guard.md): the production decision header compiled under ASan/UBSan and
driven with a fake environment, plus source guards on the call sites, the exit hook, the end-of-process discipline,
the texts and the config round trip."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
failures = []


def read(relative):
    return (root / relative).read_text(encoding='utf-8')


def require(condition, message):
    if not condition:
        failures.append(message)


def function_body(source, signature):
    """The text of one function: from its signature to the brace that closes it."""
    start = source.find(signature)
    if start < 0:
        return ''
    brace = source.index('{', start)
    depth = 0
    for i in range(brace, len(source)):
        depth += {'{': 1, '}': -1}.get(source[i], 0)
        if depth == 0:
            return source[start:i + 1]
    return ''


def literals(text):
    """C string literals in `text`, adjacent ones joined, escapes for \\n and \\" undone."""
    out = []
    for group in re.findall(r'((?:"(?:\\.|[^"\\])*"\s*)+)', text):
        out.append(''.join(re.findall(r'"((?:\\.|[^"\\])*)"', group)).replace('\\"', '"').replace('\\n', '\n'))
    return out


# --- Source guards -------------------------------------------------------------------------------------------

dllmain = read('OptiScaler/dllmain.cpp')
dx12 = read('retoque/shaders/dlssnr/DlssNr_Dx12.cpp')
vk = read('retoque/dlssnr/DlssNrFeature_Vk.cpp')
vk_host = read('retoque/dlssnr/DlssNr_VkPresentHost.cpp')
menu = read('retoque/dlssnr/DlssNr_Menu.cpp')
session = read('retoque/dlssnr/DlssNr_Session.cpp')
header = read('retoque/dlssnr/DlssNr_SessionGuard.h')
process_end = read('retoque/dlssnr/DlssNr_ProcessEnd.cpp')
ini = read('OptiScaler.ini')
note = read('docs/dlssnr/design/session-guard.md')
pack = read('retoque/translations/optional/pt-BR.lang')
vcxproj = read('OptiScaler/OptiScaler.vcxproj')
filters = read('OptiScaler/OptiScaler.vcxproj.filters')

# The upstream call sites: one line each, unchanged by version 2.
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

# Device removal, noted where NR itself sees it, inside the module only.
removed_branch = dx12[dx12.find('reportSkip("D3D12 device removed or hung");'):][:600]
require('DlssNr::SessionGuardDeviceRemoved();' in removed_branch,
        'the D3D12 evaluate does not note a removed device to the guard')
require('DlssNr::SessionGuardDeviceRemoved();' in function_body(vk_host, 'void MarkLost(DeviceTransport& dev)'),
        "the Vulkan present host's MarkLost does not note a lost device to the guard")
for upstream in ('OptiScaler/wrapped/wrapped_swapchain.cpp', 'OptiScaler/hooks/FG_Hooks.cpp', 'OptiScaler/Util.cpp'):
    require('SessionGuardDeviceRemoved' not in read(upstream), f'{upstream}: device removal is noted inside the module')

# The exit hook: ntdll's NtTerminateProcess, this process only, the first call only, everything passed through.
require('GetModuleHandleW(L"ntdll.dll")' in process_end and '"NtTerminateProcess"' in process_end,
        'the exit watch must hook ntdll!NtTerminateProcess')
is_self = function_body(process_end, 'bool IsSelf(HANDLE process)')
require('process == nullptr' in is_self and 'GetCurrentProcess()' in is_self and 'GetProcessId(process)' in is_self,
        'IsSelf must recognise NULL (ExitProcess), the pseudo-handle and a real handle to this process')
hook = function_body(process_end, 'LONG NTAPI Hook(HANDLE process, LONG status)')
require('g_fired.exchange(true)' in hook, 'the hook must act on the first end only')
require(hook.rstrip().endswith('return g_original(process, status);\n}') or 'return g_original(process, status);' in hook,
        'the hook must pass every call through')
require(not re.search(r'\bLOG_|spdlog|std::string|\bnew\b', process_end),
        'DlssNr_ProcessEnd.cpp runs at process end: no logging, no strings, no allocation')
require('DetourTransactionCommit' in process_end, 'the hook is attached through a Detours transaction')
watch = function_body(session, 'bool WatchExit()')
require('DlssNr::ProcessEnd::Watch(&OnProcessEnd)' in watch, 'WatchExit must install the hook with OnProcessEnd')
require('NoteProcessEnd(status)' in function_body(session, 'void OnProcessEnd(uint32_t status)\n{'),
        'OnProcessEnd must hand the status to the guard')

# What runs at the end of the process: no logging, no allocation of its own, no lock taken.
for signature in ('bool WriteAtExit(const char* text, size_t size)', 'bool RemoveAtExit()', 'void Pause()',
                  'bool WriteBytes(const char* text, size_t size)',
                  'static bool WriteFileBytes(const wchar_t* to, const char* text, size_t size)'):
    body = function_body(session, signature)
    require(body != '', f'DlssNr_Session.cpp: {signature} not found')
    require(not re.search(r'\bLOG_|spdlog|std::string|std::filesystem|\bnew\b|lock_guard|unique_lock', body),
            f'DlssNr_Session.cpp: {signature} runs at process end and must not log, allocate or lock')
end = function_body(header, 'void NoteProcessEnd(uint32_t status)')
require('try_lock' in end and 'lock_guard' not in end and 'Serialize(' not in end.replace('SerializeTo(', ''),
        'NoteProcessEnd must only try the lock and serialize into the fixed buffer')

# The project lists the new files in the fork-owned tail.
for name in ('DlssNr_ProcessEnd.cpp', 'DlssNr_ProcessEnd.h'):
    kind = 'ClCompile' if name.endswith('.cpp') else 'ClInclude'
    entry = f'<{kind} Include="..\\retoque\\dlssnr\\{name}"'
    require(entry in vcxproj, f'OptiScaler.vcxproj does not build {name}')
    require(entry in filters, f'OptiScaler.vcxproj.filters does not list {name}')

# The texts: every reason line and the menu's numbers formats are in the pt-BR pack and in the design note's table;
# the log lines are what the note quotes.
reason_text = function_body(header, 'inline const char* ReasonText(const Verdict& v)')
reasons = [text for text in literals(reason_text) if text]
require(len(reasons) == 10, f'expected the ten reason lines in ReasonText, found {len(reasons)}')
pack_keys = {line.split('\t')[0] for line in pack.splitlines() if line and not line.startswith('#')}
for text in reasons:
    require(text in pack_keys, f'pt-BR pack has no entry for the reason line {text!r}')
    require(text in note, f'the design note does not quote the reason line {text!r}')
for fmt in ('Exception 0x%08X after the model had run for %lld s.', 'Exit code %u after the model had run for %lld s.',
            'The model had run for %lld s.'):
    require(f'Localization::Tr("{fmt}")' in menu, f'the menu does not show {fmt!r}')
    require(fmt in pack_keys, f'pt-BR pack has no entry for {fmt!r}')
require('Localization::Tr(SessionGuardReasonText(guard))' in menu, 'the menu must show the reason for the whole verdict')
checkbox = menu[menu.index('Localization::Label("Enable Neural Rendering")'):][:600]
require('SessionGuardEnableAnyway();' in checkbox,
        'turning NR on with the checkbox must be the same statement as Enable anyway, or its notice outlives it')
for line in ('"DLSS-NR session guard: {} ({}; ran {} s, {} unclean in a row){}"',
             '"; Neural Rendering is off for this session (the ini is unchanged)"',
             '"DLSS-NR session guard: cannot see this process end (the NtTerminateProcess hook did not attach); "'):
    require(line in session, f'DlssNr_Session.cpp lost the log line {line}')
require('ran 95 s, 1 unclean in a row); Neural Rendering is off for this session (the ini is unchanged)' in
        note.replace('\n', ' '), 'the note no longer quotes the WARN line as the code writes it')

# --- The policy table ----------------------------------------------------------------------------------------

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-session-guard-') as directory:
    binary = Path(directory) / 'cases'
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(root / 'retoque'), str(here / 'cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

if failures:
    for failure in failures:
        print('FAIL:', failure)
    raise SystemExit(1)

print('PASS: session guard source guards')
