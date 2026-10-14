#!/usr/bin/env python3
"""Present-guide identity, step 0 (docs/dlssnr/design/present-real-guides.md): the production decision header on the
host, the production glue sliced out of the renderer and run against fakes, and source guards that the present pass
and the bridge still read the slot PickForPresent gave them. No GPU, no D3D12, no NGX, no game."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
retoque = root / 'retoque'

renderer = (retoque / 'shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
header = (retoque / 'dlssnr/DlssNr_PresentGuideIdentity.h').read_text()
feature = (retoque / 'dlssnr/DlssNrFeature_Dx12.h').read_text()
host = (retoque / 'dlssnr/DlssNr_PresentHost.cpp').read_text()
config = (root / 'OptiScaler/Config.h').read_text()


def between(text, start, end):
    assert text.count(start) == 1, f'production boundary changed: {start!r}'
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


def function(text, signature):
    """The body of the one definition that starts with `signature`, braces matched outside strings and comments."""
    assert text.count(signature) == 1, f'expected one definition: {signature!r}'
    i = text.index('{', text.index(signature))
    depth, j = 0, i
    while True:
        c = text[j]
        if text.startswith('//', j):
            j = text.index('\n', j)
            continue
        if c in '"\'':
            j += 1
            while text[j] != c:
                j += 2 if text[j] == '\\' else 1
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
        j += 1


def flat(text):
    return re.sub(r'\s+', ' ', text)


# --- Source guards -----------------------------------------------------------------------------------------------

# The header is pure: the standard library and the ring whose answer it narrows.
includes = sorted(l.strip() for l in header.splitlines() if l.startswith('#include'))
assert includes == sorted(['#include "DlssNr_PresentGuideRing.h"', '#include <array>', '#include <chrono>',
                           '#include <cstddef>', '#include <cstdint>', '#include <string>']), includes
assert '#include <dlssnr/DlssNr_PresentGuideIdentity.h>' in renderer

glue = between(renderer, 'struct PresentGuideIdentityState\n', 'static void ReleasePresentTemporal()')

# The glue only reads the ring: it never picks, records, invalidates or tracks a slot, never touches a clone's read
# bookkeeping, and never writes a flag the pass decides on.
for forbidden in ('PickForPresent(', 'AcquireForRecord(', 'CommitRecord(', 'InvalidateSlot(', 'Reset()', 'Submission::',
                  'readUsage', 'presentReadUsage', 'readTracked', 'writeUsage', 'g_nrLastEnhancedRecordedSeq =',
                  'g_bridgeRead', 'g_temporalValid', 'g_resetOnReturn', 'g_nr.', 'g_presentSkips'):
    assert forbidden not in glue, f'the identity glue must not touch {forbidden}'

# The observers return nothing, so there is no answer for a caller to act on.
for name in ('StampPresentGuide', 'NoteGuideIdentity', 'NotePresentPassIdentity', 'NoteBridgeIdentity'):
    assert renderer.count(f'static void {name}(') == 1, name
assert renderer.count('NoteGuideIdentity(') == 3  # its definition and the two consumers' notes

# One log line, written only when the window says one is due.
assert glue.count('LOG_') == 1
assert re.search(r'if \(consumer\.Note\([^;]*\)\)\s*\{\s*LOG_INFO\("\{\}", line\);\s*\}', glue, re.S)

# RunPresentPass: the note comes right after the pick and before today's skip; today's read still uses pickedSlot,
# and nothing in the pass looks at a decision.
present = function(renderer, 'void RunPresentPass(')
pick = ('const size_t pickedSlot = g_presentGuideRing.PickForPresent(isEligible, nowMs);\n'
        '    NotePresentPassIdentity(swapchain, device, width, height, pickedSlot);\n')
assert present.count(pick) == 1, 'the present pass notes the decision right after its pick'
assert present.count('PickForPresent(') == 1 and present.count('NotePresentPassIdentity(') == 1
skip = 'newestRecordedSeq > 0 && newestRecordedSeq == g_nrLastEnhancedRecordedSeq'
assert present.count(skip) == 1 and present.index(pick) < present.index(skip)
assert skip in function(renderer, 'static void NotePresentPassIdentity('), 'the note and the pass skip on one test'
for today in ('if (pickedSlot < DlssNr::PresentGuides::kRingSize)', 'auto& clone = g_presentGuideClones[pickedSlot];',
              'trackedSlot = pickedSlot;', 'temporalSeq = g_presentGuideRing.GetSlot(pickedSlot).sequence;',
              'g_nrLastEnhancedRecordedSeq = newestRecordedSeq;'):
    assert present.count(today) == 1, f'today\'s read changed: {today}'
# Whole words, so another decision in the pass (the HDR present's PresentColourDecision) is not mistaken for this one,
# nor PresentBoundary::Decide, the route's own decision about the game's DLSS-G (DlssNr_PresentBoundary.h).
for decision in (r'(?<!PresentBoundary::)\bDecide\(', r'\bDecision\b', r'\bg_guideIdentity\b',
                 r'\bPresentGuideIdentity::'):
    assert not re.search(decision, present), f'RunPresentPass reads the decision: {decision}'

# At the default HookMethod (1) and with the pass off, RunPresentPass returns before the pick.
assert 'CustomOptional<uint32_t> DlssNrHookMethod { 1 };' in config
assert present.index('if (!cfg.DlssNrEnabled.value_or_default())') < present.index(pick)
# The placement is decided by DlssNr_PresentBoundary.h (HookMethod=1 is Upscaled there, tests/nr-present-boundary).
assert present.index('if (verdict == DlssNr::PresentBoundary::PresentVerdict::Upscaled)') < present.index(pick)
assert present.index('if (verdict == DlssNr::PresentBoundary::PresentVerdict::Declined)') < present.index(pick)

# CapturedPresentGuides: the same for the bridge.
bridge = function(renderer, 'bool CapturedPresentGuides(')
bridge_pick = ('const size_t pickedSlot = g_presentGuideRing.PickForPresent(isEligible, NowMs());\n'
               '    NoteBridgeIdentity(cmdList, presenter, presentWidth, presentHeight, pickedSlot);\n')
assert bridge.count(bridge_pick) == 1, 'the bridge notes the decision right after its pick'
for today in ('if (pickedSlot >= DlssNr::PresentGuides::kRingSize)\n        return false;',
              'auto& clone = g_presentGuideClones[pickedSlot];',
              'g_bridgeRead = { true, pickedSlot, cmdList, savedUsage, savedPresentUsage, savedTracked };'):
    assert bridge.count(today) == 1, f'the bridge\'s read changed: {today}'
for decision in ('Decide(', 'Decision', 'g_guideIdentity', 'PresentGuideIdentity::'):
    assert decision not in bridge, f'CapturedPresentGuides reads the decision: {decision}'
bridge_note = function(renderer, 'static void NoteBridgeIdentity(')
assert '!cfg.DlssNrEnabled.value_or_default() || cfg.DlssNrHookMethod.value_or_default() != 2' in bridge_note
assert ('ID3D12CommandQueue* queue = nullptr, const void* presenter = nullptr, unsigned int presentWidth = 0, '
        'unsigned int presentHeight = 0);') in flat(feature)
assert flat(host).count('CapturedPresentGuides(cmdList, &capturedDepth, &capturedMotion, &captured, queue, this, '
                        'sourceWidth, sourceHeight)') == 1

# CaptureTemporal: one stamp, after both copies are recorded and right before the commit. It runs only where the
# capture does: HookMethod 2, or 0 with the present hook live.
capture = function(renderer, 'static void CaptureTemporal(')
stamp = ('StampPresentGuide(slot, params, frame, devId, clone.width, clone.height);\n'
         '    g_presentGuideRing.CommitRecord(')
assert capture.count('StampPresentGuide(') == 1 and capture.count(stamp) == 1
assert capture.index('cmdList->CopyTextureRegion(&dstMotion') < capture.index(stamp)
assert renderer.count('StampPresentGuide(') == 2  # its definition and its one call
assert renderer.count('CaptureTemporal(cmdList, params, frame);') == 1
# The capture happens only when a present pass would use it, decided by DlssNr_PresentBoundary.h.
assert 'if (DlssNr::PresentBoundary::UpscaledCapturesForPresent(boundary, PresentHookLive()))\n' \
       '        CaptureTemporal(cmdList, params, frame);' in renderer

# Shutdown forgets the identity together with the ring.
release = function(renderer, 'static void ReleasePresentTemporal(')
assert release.index('g_presentGuideRing.Reset();') < release.index('g_guideIdentity = {};')

print('PASS: source guards -- pure header, the glue only reads the ring, observers return nothing, one gated log '
      'line; RunPresentPass and CapturedPresentGuides read the slot PickForPresent gave them and never a decision; '
      'stamp before commit; inert at HookMethod=1, off, and on an empty ring', flush=True)

# --- The header's cases, then the glue as the renderer has it --------------------------------------------------

flags = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', '-I', str(retoque), '-I', str(root / 'OptiScaler')]

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-present-guide-identity-') as directory:
    directory = Path(directory)

    decide = directory / 'decide'
    subprocess.run(flags + [str(here / 'decide.cpp'), '-o', str(decide)], check=True)
    subprocess.run([str(decide)], check=True)

    unit = directory / 'glue.cpp'
    unit.write_text((here / 'glue_fakes.h').read_text() + '\nnamespace DlssNr\n{\n' + glue + '\n} // namespace DlssNr\n' +
                    (here / 'glue_cases.h').read_text())
    binary = directory / 'glue'
    subprocess.run(flags + ['-Wno-unused-function', str(unit), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
