#!/usr/bin/env python3
"""Present-guide identity, step 1 (docs/dlssnr/design/present-real-guides.md): the production decision header on the
host, the production glue sliced out of the renderer and run against fakes, and source guards that the present pass
and the bridge read only the slot the identity rules return. No GPU, no D3D12, no NGX, no game."""
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

# The glue reads the ring and never changes it: it never picks, records, invalidates or tracks a slot, never touches a
# clone's read bookkeeping, and never writes a flag the pass decides on. Its one write outside its own state is the
# model reset a lasting source change owes (D3), in NoteGuideSource only.
for forbidden in ('PickForPresent(', 'AcquireForRecord(', 'CommitRecord(', 'InvalidateSlot(', 'g_presentGuideRing.Reset',
                  'Submission::', 'readUsage', 'presentReadUsage', 'readTracked', 'writeUsage',
                  'g_nrLastEnhancedRecordedSeq', 'g_bridgeRead', 'g_presentGuideClones', 'g_temporalValid', 'g_nr.',
                  'g_presentSkips'):
    assert forbidden not in glue, f'the identity glue must not touch {forbidden}'
assert glue.count('g_resetOnReturn') == 1
source_note = function(renderer, 'static bool NoteGuideSource(')
assert 'if (!history.Note(game))\n        return history.Established() == 1;\n\n    g_resetOnReturn = true;' in source_note
# The bridge's list is not asked for its device on a ring that never recorded.
bridge_ask_fn = function(renderer, 'static GuideAsk AskBridgeGuides(')
assert bridge_ask_fn.index('if (g_presentGuideRing.NewestRecordedSequence() == 0)\n        return {};') < \
    bridge_ask_fn.index('GetDevice(')
# A use moves rule 5's record whether or not the present is logged.
count_fn = function(renderer, 'static void CountGuideIdentity(')
assert 'consumer.MarkUsed(g_presentGuideRing.GetSlot(picked).sequence);' in count_fn

# A ring that never recorded is not asked: no boundary, no presenter, no decision, no source.
ask_fn = function(renderer, 'static GuideAsk AskGuideIdentity(')
assert ask_fn.index('if (g_presentGuideRing.NewestRecordedSequence() == 0)\n        return ask;') < ask_fn.index('AtPresent(')
assert 'ask.present.enforceLag = enforceLag;' in ask_fn
assert 'return AskGuideIdentity(g_guideIdentity.present, device, presenter, width, height, picked, boundary);' in \
    function(renderer, 'static GuideAsk AskPresentPassGuides(')
assert 'return AskGuideIdentity(g_guideIdentity.bridge, device, presenter, width, height, picked, false);' in \
    function(renderer, 'static GuideAsk AskBridgeGuides(')

# Two log calls, each a preformatted line: the window's, when Note says one is due, and the source change's, rationed.
assert glue.count('LOG_') == 2
assert re.search(r'consumer\.Note\([^;]*\)\)\s*\{\s*LOG_INFO\("\{\}", line\);\s*\}', glue, re.S)
assert re.search(r'if \(\+\+resets <= 3 \|\| resets % 100 == 0\)\s*\{\s*LOG_INFO\("\{\}",', source_note, re.S)

# RunPresentPass: the identity is asked right after the pick, and what the pass reads is the slot it answers -- never
# the pick itself, so a refused capture is never tracked or read. The skip stays, before any read, and is counted.
present = function(renderer, 'void RunPresentPass(')
pick = ('const size_t pickedSlot = g_presentGuideRing.PickForPresent(isEligible, nowMs);\n')
ask = ('const GuideAsk guides = AskPresentPassGuides(device, presenter, width, height, pickedSlot, boundary);\n'
       '    const size_t usableSlot = guides.decision.slot;\n')
assert present.count(pick) == 1 and present.count('PickForPresent(') == 1
assert present.count(ask) == 1 and present.index(pick) < present.index(ask)
assert present.count('AskPresentPassGuides(') == 1
assert 'const void* const presenter = PresentGuidePresenter(swapchain);' in present
assert present.index('PresentGuidePresenter(swapchain)') < present.index('std::unique_lock<std::mutex> nrLock(g_nrMutex);')
# pickedSlot is handed to the identity and the record only, and read for its sequence when it was refused.
assert len(re.findall(r'\bpickedSlot\b', present)) == 5, re.findall(r'.*\bpickedSlot\b.*', present)
for read in ('if (usableSlot < DlssNr::PresentGuides::kRingSize)', 'auto& clone = g_presentGuideClones[usableSlot];',
             'trackedSlot = usableSlot;', 'temporalSeq = g_presentGuideRing.GetSlot(usableSlot).sequence;'):
    assert present.count(read) == 1, f'the read changed: {read}'
assert len(re.findall(r'g_presentGuideClones\[', present)) == present.count('g_presentGuideClones[usableSlot]') + \
    present.count('g_presentGuideClones[trackedSlot]')
skip = 'newestRecordedSeq > 0 && newestRecordedSeq == g_nrLastEnhancedRecordedSeq'
assert present.count(skip) == 1 and present.index(ask) < present.index(skip)
skip_body = present[present.index(skip):present.index('if (usableSlot < DlssNr::PresentGuides::kRingSize)')]
assert 'CountPresentPassGuides(guides, pickedSlot, DlssNr::PresentGuideIdentity::Outcome::Skipped);' in skip_body
assert 'return;' in skip_body
count = ('const auto guideOutcome = DlssNr::PresentGuideIdentity::OutcomeOf(guides.decision, temporalValid);\n'
         '    CountPresentPassGuides(guides, pickedSlot, guideOutcome);')
assert present.count(count) == 1
assert present.index('trackedSlot = usableSlot;') < present.index(count) < present.index('nrLock.unlock();\n\n    static ZeroGuides')
# The newest capture refused is decided on, like one used: the skip above then covers the presents after it. Only the
# newest: a refused older pick leaves a newer capture to the next present.
mark = ('if (guideOutcome == DlssNr::PresentGuideIdentity::Outcome::Refused &&\n'
        '        g_presentGuideRing.GetSlot(pickedSlot).sequence == newestRecordedSeq)\n'
        '    {\n        g_nrLastEnhancedRecordedSeq = newestRecordedSeq;\n    }')
assert present.count(mark) == 1 and present.index(count) < present.index(mark)
assert present.count('g_nrLastEnhancedRecordedSeq = newestRecordedSeq;') == 2
source = ('if (temporalValid || !cfg.DlssNrRequireDlss.value_or_default())\n'
          '        gameGuidesHold = NotePresentPassGuideSource(temporalValid, !g_sourceIsPresent.load());')
assert present.count(source) == 1 and present.index(count) < present.index(source)
assert present.index(source) < present.index('if (!g_sourceIsPresent.exchange(true))')
# ZeroGuideReset waits while the game's guides hold the history; the RequireDlss skip says it waited.
assert present.count('frame.Reset = cfg.DlssNrZeroGuideReset.value_or_default() && !gameGuidesHold;') == 1
waiting = present[present.index('if (!temporalValid && cfg.DlssNrRequireDlss.value_or_default())'):]
waiting = waiting[:waiting.index('return;')]
assert 'NotePresentGuideWaiting();' in waiting
# The route the status names is the present entry's own word, set once the placement chose it.
assert present.count('g_presentRouteAtMs = NowMs();') == 1
assert present.index('if (verdict == DlssNr::PresentBoundary::PresentVerdict::Declined)') < \
    present.index('g_presentRouteAtMs = NowMs();') < present.index(pick)
# What a frame without the game's guides ran on, after the synthesized motion is recorded.
fallback = 'if (!temporalValid)\n        NotePresentGuideFallback(motion != s_dummyGuides.Motion());'
assert present.count(fallback) == 1
assert present.index('g_presentMotion->Record(') < present.index(fallback) < present.index('EvaluateAtPresent(')

# At the default HookMethod (1) and with the pass off, RunPresentPass returns before the pick.
assert 'CustomOptional<uint32_t> DlssNrHookMethod { 1 };' in config
assert present.index('if (!cfg.DlssNrEnabled.value_or_default())') < present.index(pick)
# The placement is decided by DlssNr_PresentBoundary.h (HookMethod=1 is Upscaled there, tests/nr-present-boundary).
assert present.index('if (verdict == DlssNr::PresentBoundary::PresentVerdict::Upscaled)') < present.index(pick)
assert present.index('if (verdict == DlssNr::PresentBoundary::PresentVerdict::Declined)') < present.index(pick)

# CapturedPresentGuides: the same for the bridge. It reads the slot the rules return through ReadCapturedGuides, which
# tracks the read before handing anything out.
bridge = function(renderer, 'bool CapturedPresentGuides(')
bridge_pick = 'const size_t pickedSlot = g_presentGuideRing.PickForPresent(isEligible, NowMs());\n'
bridge_ask = ('const GuideAsk guides = AskBridgeGuides(cmdList, presenter, presentWidth, presentHeight, pickedSlot);\n'
              '    const bool read = ReadCapturedGuides(guides.decision.slot, cmdList, depth, motion, frame);\n')
assert bridge.count(bridge_pick) == 1 and bridge.count(bridge_ask) == 1
assert bridge.index(bridge_pick) < bridge.index(bridge_ask)
assert len(re.findall(r'\bpickedSlot\b', bridge)) == 3  # the pick, the ask, the record
assert 'g_presentGuideClones' not in bridge and 'Submission::Track' not in bridge
assert ('CountBridgeGuides(guides, pickedSlot, DlssNr::PresentGuideIdentity::OutcomeOf(guides.decision, read), '
        'presenter);') in flat(bridge)
read_fn = function(renderer, 'static bool ReadCapturedGuides(')
assert read_fn.index('if (slot >= DlssNr::PresentGuides::kRingSize)\n        return false;') < read_fn.index('Track(')
assert read_fn.index('Submission::Track(cmdList, clone.presentReadUsage)') < read_fn.index('*depth = clone.depth;')
assert 'g_bridgeRead = { true, slot, cmdList, savedUsage, savedPresentUsage, savedTracked };' in read_fn
bridge_count = function(renderer, 'static bool CountBridgeGuides(')
assert ('presenter != nullptr && cfg.DlssNrEnabled.value_or_default() && cfg.DlssNrHookMethod.value_or_default() == 2'
        in flat(bridge_count))
assert ('ID3D12CommandQueue* queue = nullptr, const void* presenter = nullptr, unsigned int presentWidth = 0, '
        'unsigned int presentHeight = 0, bool* gameGuidesHold = nullptr);') in flat(feature)
assert flat(host).count('CapturedPresentGuides(cmdList, &capturedDepth, &capturedMotion, &captured, queue, this, '
                        'sourceWidth, sourceHeight, &gameGuidesHold)') == 1
assert ('if (motion == _guides.Motion() && Config::Instance()->DlssNrZeroGuideReset.value_or_default() && '
        '!gameGuidesHold)') in flat(host)
assert '*gameGuidesHold = hold;' in bridge
assert 'if (capturedMotion == nullptr)\n        NotePresentGuideFallback(motion != _guides.Motion());' in host

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

# The status line names the guides from the decisions of the last second, never from g_temporalValid (D3); the menu's
# guides line sits with it, and only a present host's decisions publish it.
status = function(renderer, 'const char* HookStatus(')
assert 'g_temporalValid' not in re.sub(r'//[^\n]*', '', status)
assert status.count('RecentGuideStatus(&guides)') == 1 and status.count('RouteText(') == 2
assert 'if (presenting && PresentRouteLive())' in status
assert 'waiting for DLSS temporal inputs' not in status  # one wording for waiting: RouteText's
assert 'swapchain source with DLSS temporal inputs' not in renderer
menu = (retoque / 'dlssnr/DlssNr_Menu.cpp').read_text()
assert menu.count('DlssNr::PresentGuidesLine()') == 1
assert menu.index('const char* status = DlssNr::HookStatus();') < menu.index('DlssNr::PresentGuidesLine()')
assert glue.count('g_guideStatus = ') == 1 and 'g_guideStatus = ' in function(renderer, 'static void CountGuideIdentity(')

print('PASS: source guards -- pure header; the glue reads the ring and writes only the reset a held source change '
      'owes; RunPresentPass and CapturedPresentGuides read the slot the rules return, never the pick, and track it '
      'before use; the skip stays and is counted; rule 6 enforced at the boundary only; the status reads no latch; '
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
