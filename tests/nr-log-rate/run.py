#!/usr/bin/env python3
"""Behaviour of the production NR log rate limiter, plus the call sites that use it."""
from pathlib import Path
import subprocess
import tempfile
import re

here = Path(__file__).resolve().parent
root = here.parents[1]
source = (root / 'OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp').read_text()


def report(name):
    start = source.index(f'struct {name}\n')
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end + 1]


def initializer(start):
    begin = source.index(start)
    return source[begin:source.index(';', begin) + 1]


# Compile the actual classification AND the actual field wiring. A stand-in
# Report cannot catch a forgotten config dependency in ComposeReport.
sites = r'''
#include <dlssnr/DlssNr_LogRate.h>
#include <dlssnr/DlssNr_Exposure.h>
#include <shaders/dlssnr/DlssNr_Common.h>
#include <iostream>
#include <string>
#include <vector>
namespace DlssNr::ExposureScan {
    struct AnchorPoint { float scan, white; };
    inline std::vector<AnchorPoint> anchors;
    inline std::vector<AnchorPoint> Anchors() { return anchors; }
}
using namespace DlssNr;
template<class T> struct Value { T value {}; T value_or_default() const { return value; } };
struct ComposeInputs {
    int whitePointSource = 0;
    float whitePointScale = 1, whitePointTrim = 1, scanTrim = 1, gameExposure = 1, detail = 1;
    bool scanInverted = false, holdFrame = false;
    struct { Value<bool> DlssNrScanExposure; } cfg;
    std::vector<ExposureScan::AnchorPoint> anchors { { 1, 1 } };
    struct { unsigned workWidth = 1720, workHeight = 720; } g_nr;
};
struct ComposeSite : ComposeInputs {
''' + report('ComposeReport') + r'''
    LogRate::Reporter<ComposeReport> reporter;
    ComposeInputs& inputs() { return *this; }
    bool Observe(LogRate::Clock::time_point time) {
        ExposureScan::anchors = anchors;
        DlssNrConstants resolveParams {};
        resolveParams.WhitePoint = Exposure::WhitePoint(whitePointSource, true, whitePointScale,
            whitePointTrim, gameExposure, 1.0f, scanTrim);
        resolveParams.TransferStrength = detail;
        resolveParams.ColourStrength = 1;
        resolveParams.MaxRatio = 2;
''' + initializer('const ComposeReport composeNow') + r'''
        return reporter.Observe(composeNow, time);
    }
};
struct ExposureSite {
    float preExposure = 1;
    bool havePre = true, autoExposureFlag = false;
    void* exposureTex = this;
''' + report('ExposureReport') + r'''
    LogRate::Reporter<ExposureReport> reporter;
    bool Observe(LogRate::Clock::time_point time) {
''' + initializer('const ExposureReport now') + r'''
        return reporter.Observe(now, time);
    }
};
'''

# Scalar sites before the fix, structured reports afterward. In both cases the
# input expression/initializer comes from the real call site, not a copied policy.
for cls, report_name, variable, reporter, fields in [
    ('ExposureValueSite', 'ExposureValueReport', 'exposureNow', 'exposureValue',
     'struct { float gameExposure = 1, gamePreExposure = 1; } g_nr;'),
    ('ScanSite', 'ScanReport', 'scanNow', 'scanValue',
     'float scanned = 1, low = 0.5f, high = 2; int which = 1; '
     'struct { float gameExposure = 1; } g_nr;'),
]:
    if f'struct {report_name}\n' in source:
        declaration = report(report_name)
        init = initializer(f'const {report_name} {variable}')
    else:
        declaration = f'using {report_name} = LogRate::Drift;'
        expr = re.search(rf'{reporter}\.Observe\((.*), LogRate::Clock::now\(\)\)', source)[1]
        init = f'const {report_name} {variable} = {expr};'
    sites += f'''struct {cls} {{
        {fields}
        {declaration}
        LogRate::Reporter<{report_name}> reporter;
        bool Observe(LogRate::Clock::time_point time) {{
            {init}
            return reporter.Observe({variable}, time);
        }}
    }};
'''
sites = ('namespace DlssNr::ExposureScan { inline bool enabled = false; '
         'inline bool Scanning() { return enabled; } }\n') + sites
sites += (here / 'site_cases.cpp').read_text()

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-log-sites-') as directory:
    cpp = Path(directory) / 'sites.cpp'
    binary = Path(directory) / 'sites'
    cpp.write_text(sites)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I' + str(root / 'OptiScaler'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-log-rate-') as directory:
    binary = Path(directory) / 'rate'
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I' + str(root / 'OptiScaler'), str(here / 'rate.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(here / 'crimson-compose.trace')], check=True)

# The header only decides; these are the sites that have to ask it. A limiter nothing calls is a
# limiter that changed nothing.

assert '#include <dlssnr/DlssNr_LogRate.h>' in source, 'the limiter is not included'
assert source.count('Reporter<') == 4, 'expected the four drift sites to hold a reporter'

# `Observe` is also the name the extent-settling helpers in this file use, so count the reporters by
# name rather than by the call.
for reporter in ('composeReporter', 'exposureReporter', 'exposureValue', 'scanValue'):
    assert source.count(f'{reporter}.Observe(') == 1, f'{reporter} must be asked exactly once'

# Each one is given the monotonic clock. A wall clock can step backwards and silence a site.
assert source.count('LogRate::Clock::now()') == 4, 'every reporter reads the monotonic clock'
assert 'system_clock' not in source

for name in ('loggedCompose', 'loggedExposure', 'loggedScan'):
    assert name not in source, f'{name} survived as a live reader'

body = source.split('const ComposeReport composeNow', 1)[1].split('Barrier(cmdList', 1)[0]
assert 'composeReporter.Observe(composeNow, DlssNr::LogRate::Clock::now())' in body
assert body.index('composeReporter.Observe') < body.index('LOG_INFO'), 'the line must be gated, not merely counted'

# Nothing the GPU or the timing evidence depends on goes through a rate limiter.
timing = (root / 'OptiScaler/dlssnr/DlssNr_GpuTiming.cpp').read_text()
assert 'LogRate' not in timing, 'GPU timing samples must not be rate limited'
assert 'gpu timing confirmed' in timing

print('PASS: four drift sites gated, composition shape classified, GPU timing untouched')
