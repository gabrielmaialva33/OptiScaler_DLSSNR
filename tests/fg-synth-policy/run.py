#!/usr/bin/env python3
"""Synthesized FG's policy (inputs/FG/Synth_Policy.h), its motion statistics (inputs/FG/Synth_MotionStats.h),
its HUD plan (inputs/FG/Synth_Hud.h) and the NR/FG motion handoff (shaders/synth_motion/SynthMotion_Handoff.h),
compiled from the production headers under ASan/UBSan. Also holds the HUD mask's thresholds equal to
DLSS-NR's, which they copy."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parent
repo = root.parents[1]

# Synthesized FG's HUD mask runs DLSS-NR's static-overlay rule with DLSS-NR's thresholds (SynthOverlay_Dx12.cpp
# says so); two copies of one list drift unless something holds them together.
THRESHOLD = re.compile(r'constexpr\s+(?:float|uint32_t)\s+(k[A-Z]\w*)\s*=\s*([0-9.]+)f?\s*;')
names = ('kStaticEps', 'kCoreEps', 'kMotionTau', 'kDetailMin', 'kDecay', 'kDropTau', 'kStreakMin', 'kSupportMin',
         'kSidesMin')
nr = dict(THRESHOLD.findall((repo / 'OptiScaler/shaders/dlssnr/DlssNr_UiMask_Dx12.cpp').read_text()))
fg = dict(THRESHOLD.findall((repo / 'OptiScaler/shaders/synth_motion/SynthOverlay_Dx12.cpp').read_text()))
missing = [n for n in names if n not in nr or n not in fg]
differ = [f'{n}: DLSS-NR {nr[n]}, frame generation {fg[n]}' for n in names
          if n in nr and n in fg and float(nr[n]) != float(fg[n])]
if missing or differ:
    for n in missing:
        print(f'FAIL: threshold {n} not found in both mask passes')
    for d in differ:
        print(f'FAIL: HUD mask threshold differs, {d}')
    sys.exit(1)
print(f'PASS: the HUD mask\'s {len(names)} thresholds equal DLSS-NR\'s')

# The rule's shader includers pass it texture loads through function-like macros. A macro substitutes every
# token that matches a parameter, a swizzle included: `#define UM_PROT(x, y) Load(int2(x, y)).x` called as
# UM_PROT(x + sx, y + sy) reads `.x + sx`, the support and growth loops count every neighbour as protected, and
# the mask is 1 on every pixel. The host tests cannot see it (their macros are plain calls); the GPU harness saw
# it on 2026-09-28. DLSS-NR's pass had it until its macros were renamed the same day (hud-protection.md,
# "The mask macros: every pixel protected"); no includer is excused now.
KNOWN_SWIZZLE_HAZARD = set()
DEFINE = re.compile(r'^\s*#\s*define\s+(UM_\w+)\(([^)]*)\)(.*)$', re.M)
includers = [p for p in (repo / 'OptiScaler/shaders').rglob('*.hlsl')
             if 'static_overlay_rule.h' in p.read_text(errors='replace')]
if len(includers) < 2:
    print(f'FAIL: expected the rule\'s two shader includers, found {[str(p) for p in includers]}')
    sys.exit(1)
for path in includers:
    relative = str(path.relative_to(repo))
    hazards = []
    for name, params, body in DEFINE.findall(path.read_text(errors='replace')):
        for param in (p.strip() for p in params.split(',')):
            if param and re.search(rf'\.\s*{re.escape(param)}\b', body):
                hazards.append(f'{name}: .{param}')
    if hazards and relative in KNOWN_SWIZZLE_HAZARD:
        print(f'NOTE: {relative} still has the swizzle-parameter hazard ({", ".join(hazards)}); known, not fixed here')
    elif hazards:
        print(f'FAIL: {relative}: a macro parameter is also a swizzle in its body ({", ".join(hazards)}); '
              'the rule then reads the swizzle plus the offset')
        sys.exit(1)
    elif relative in KNOWN_SWIZZLE_HAZARD:
        print(f'FAIL: {relative} no longer has the swizzle hazard: drop it from KNOWN_SWIZZLE_HAZARD')
        sys.exit(1)
print(f'PASS: no unexpected swizzle-parameter hazard in the rule\'s {len(includers)} shader includers')

# Frame generation's mask runs the rule with a 3x3 still core (UM_CORE_RADIUS in its shader). The fixtures that
# made DLSS-NR's 5x5 core necessary -- sand beside a swaying coat, a concave gap -- must still mark nothing
# there, and a glyph over a pan must still be protected: tests/nr-uimask-rule's own cases, at that radius.
detect = (repo / 'OptiScaler/shaders/synth_motion/precompile/synth_overlay_detect.hlsl').read_text()
radius = re.search(r'^\s*#\s*define\s+UM_CORE_RADIUS\s+(\d+)', detect, re.M)
if radius is None:
    print('FAIL: synth_overlay_detect.hlsl no longer defines UM_CORE_RADIUS')
    sys.exit(1)

with tempfile.TemporaryDirectory(prefix='optiscaler-fg-synth-policy-') as directory:
    rule = str(Path(directory) / 'rule')
    subprocess.run(['g++', '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-g', f'-DUM_CORE_RADIUS={radius.group(1)}',
                    '-I', str(repo / 'OptiScaler'), str(repo / 'tests/nr-uimask-rule/test.cpp'), '-o', rule],
                   check=True)
    print(f'rule fixtures at frame generation\'s core radius {radius.group(1)}:', flush=True)
    subprocess.run([rule], check=True)

    binary = str(Path(directory) / 'test')
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-g', '-I', str(repo / 'OptiScaler'),
                    str(root / 'test.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True)
