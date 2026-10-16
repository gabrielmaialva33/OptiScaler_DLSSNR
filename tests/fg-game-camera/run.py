#!/usr/bin/env python3
"""Compile production camera construction and input-frame mapping with real Streamline types."""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
repo = here.parents[1]
source = (repo / 'OptiScaler/framegen/dlssg/DLSSG_Common.cpp').read_text()
dx12 = (repo / 'OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp').read_text()


def function(signature, text):
    begin = text.index(signature)
    start = text.index('{', begin)
    depth, end = 1, start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[begin:end]


begin = source.index('    sl::Constants constData = {};')
end = source.index('    constData.jitterOffset.x', begin)
camera = source[begin:end]
# Keep the real forwarding statements and actual SetConstants argument; unrelated token/GPU work is fake.
extra = ''
if '_gameCamera.Apply' in source:
    line = next(line for line in source.splitlines() if '_gameCamera.Apply' in line)
    extra = line + '\n'
forward = next(line for line in source.splitlines() if 'auto result = StreamlineProxy::SetConstants()' in line)
harness = (here / 'test.cpp').read_text()
frame_source = (repo / 'OptiScaler/framegen/IFGFeature.cpp').read_text()
frame_slices = ''
for signature in ('int IFGFeature::GetIndex()', 'UINT64 IFGFeature::StartNewFrame()',
                  'int IFGFeature::GetDispatchIndex(', 'bool IFGFeature::IsActive()',
                  'bool IFGFeature::IsPaused()', 'bool IFGFeature::IsDispatched()',
                  'void IFGFeature::SetFrameCount(', 'UINT64 IFGFeature::FrameCount()'):
    frame_slices += function(signature, frame_source) + '\n'
harness = harness.replace('// @frame-counter@', frame_slices)
inputs = (repo / 'OptiScaler/inputs/FG/Streamline_Inputs.cpp').read_text()
header = (repo / 'OptiScaler/inputs/FG/Streamline_Inputs.h').read_text()
declarations = header[header.index('    bool infiniteDepth'):header.index('  public:')]
mapping = 'class Sl_Inputs { public:\n' + declarations + '\nvoid markPresent(uint64_t frameId);\n};\n'
for signature in ('void Sl_Inputs::CheckForFrame(', 'int Sl_Inputs::IndexForFrameId(',
                  'void Sl_Inputs::CaptureGameCamera(', 'void Sl_Inputs::markPresent('):
    mapping += function(signature, inputs) + '\n'
harness = harness.replace('// @mapping@', mapping)
for name, tail in [('Deactivate', '    if (_isActive)'), ('ReleaseObjects', '    for (size_t')]:
    body = function('void DLSSG_' + ('Common' if name == 'Deactivate' else 'Dx12') + '::' + name + '()', source if name == 'Deactivate' else dx12)
    body = body[body.index('{') + 1:body.index(tail)]
    harness = harness.replace('// @' + name + '@', body)
assert 'CaptureGameCamera(fgOutput, frameId, data, dataFound);' in inputs
harness = harness.replace('// @camera@', camera + '''
    constData.mvecScale = { 0.5f, 0.25f };
    constData.reset = sl::Boolean::eTrue;
    constData.motionVectorsDilated = sl::Boolean::eTrue;
''' + extra + '''
    Token token; auto* _frameToken = &token; int _viewport = 0;
''' + forward + '\n    (void)result; return received;')
with tempfile.TemporaryDirectory(prefix='retoque-fg-camera-') as directory:
    cpp = Path(directory) / 'test.cpp'
    cpp.write_text(harness)
    binary = Path(directory) / 'test'
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-type-limits', '-pthread',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I', str(repo / 'external/streamline'),
                    '-I', str(repo / 'retoque'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
