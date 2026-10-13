#!/usr/bin/env python3
"""The comparison capture's host checks (docs/dlssnr/design/comparison-capture.md, section 6, host tier).

Builds test.cpp against the production headers (DlssNr_ImageFile.h, DlssNr_PairCapture.h) under ASan/UBSan and runs
it: the state machine over a fake backend, the menu button, names, the cap, the clock, the record and the
write-then-rename are asserted there. The files it writes are checked here, from the other side: every PNG is walked
chunk by chunk (CRC-32 with zlib.crc32), its zlib stream block by block (stored blocks, LEN/NLEN, Adler-32 with
zlib.adler32) and decompressed with zlib; every pixel of every PNG and PFM is recomputed in Python from the source
surface the C++ side wrote, padding included, with half floats decoded by struct. Then the repository's privacy
patterns are run over every record generated, and source guards hold the integration in place.

No GPU, no D3D12, no Wine: what this does not cover is listed in README.md.
"""
from pathlib import Path
import json
import math
import re
import struct
import subprocess
import sys
import tempfile
import zlib

root = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
failures = []


def fail(message):
    failures.append(message)


def f32_bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def half_bits_to_f32_bits(h):
    sign, exponent, mantissa = (h >> 15) & 1, (h >> 10) & 0x1F, h & 0x3FF
    if exponent == 31 and mantissa != 0:
        return (sign << 31) | 0x7F800000 | (mantissa << 13)  # NaN, payload kept
    return f32_bits(struct.unpack('<e', struct.pack('<H', h))[0])


def small_bits_to_f32_bits(v, mantissa_bits):
    mantissa, exponent = v & ((1 << mantissa_bits) - 1), (v >> mantissa_bits) & 0x1F
    if exponent == 31:
        return 0x7F800000 | (mantissa << (23 - mantissa_bits))
    if exponent == 0:
        value = mantissa * 2.0 ** (-14 - mantissa_bits)
    else:
        value = (1 + mantissa / (1 << mantissa_bits)) * 2.0 ** (exponent - 15)
    return f32_bits(value)


def bits_to_float(bits):
    return struct.unpack('<f', struct.pack('<I', bits))[0]


def quantise8(bits):
    """The production rule on float32 bits: NaN and anything negative to 0, one and above to 255, else round."""
    if (bits & 0x7FFFFFFF) > 0x7F800000 or bits & 0x80000000:
        return 0
    if bits >= 0x3F800000:
        return 255
    value = bits_to_float(bits)
    return int(f32(f32(value * 255.0) + 0.5))


SOURCES = {
    27: 'rgba8', 28: 'rgba8', 29: 'rgba8', 87: 'bgra8', 90: 'bgra8', 91: 'bgra8', 23: 'rgb10a2', 24: 'rgb10a2',
    9: 'rgba16f', 10: 'rgba16f', 2: 'rgba32f', 1: 'rgba32f', 26: 'rg11b10f',
}
BPP = {'rgba8': 4, 'bgra8': 4, 'rgb10a2': 4, 'rgba16f': 8, 'rgba32f': 16, 'rg11b10f': 4}


def pixel_float_bits(source, px):
    """The three channels of a float pixel as float32 bits, decoded here rather than by the code under test."""
    if source == 'rgba16f':
        return [half_bits_to_f32_bits(struct.unpack_from('<H', px, 2 * c)[0]) for c in range(3)]
    if source == 'rgba32f':
        return [struct.unpack_from('<I', px, 4 * c)[0] for c in range(3)]
    v = struct.unpack_from('<I', px, 0)[0]
    return [small_bits_to_f32_bits(v & 0x7FF, 6), small_bits_to_f32_bits((v >> 11) & 0x7FF, 6),
            small_bits_to_f32_bits((v >> 22) & 0x3FF, 5)]


def read_png(path):
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise AssertionError(f'{path.name}: no PNG signature')
    pos, chunks = 8, []
    while pos < len(data):
        length = struct.unpack('>I', data[pos:pos + 4])[0]
        kind, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + length]
        crc = struct.unpack('>I', data[pos + 8 + length:pos + 12 + length])[0]
        if zlib.crc32(kind + body) & 0xFFFFFFFF != crc:
            raise AssertionError(f'{path.name}: {kind} chunk CRC')
        chunks.append((kind, body))
        pos += 12 + length
    if pos != len(data):
        raise AssertionError(f'{path.name}: trailing bytes')
    kinds = [k for k, _ in chunks]
    if kinds[0] != b'IHDR' or kinds[-1] != b'IEND' or chunks[-1][1] != b'' or kinds.count(b'IEND') != 1:
        raise AssertionError(f'{path.name}: chunk order {kinds[:3]}...{kinds[-2:]}')
    idats = [i for i, k in enumerate(kinds) if k == b'IDAT']
    if not idats or idats != list(range(idats[0], idats[-1] + 1)):
        raise AssertionError(f'{path.name}: IDAT chunks not consecutive')
    if any(k in (b'tEXt', b'sRGB') and i > idats[0] for i, k in enumerate(kinds)):
        raise AssertionError(f'{path.name}: tEXt or sRGB after the image data')
    width, height, depth, colour, compression, filtering, interlace = struct.unpack('>IIBBBBB', chunks[0][1])
    if colour != 2 or compression or filtering or interlace or depth not in (8, 16):
        raise AssertionError(f'{path.name}: IHDR {colour} {compression} {filtering} {interlace} {depth}')

    stream = b''.join(body for kind, body in chunks if kind == b'IDAT')
    if stream[0] != 0x78 or stream[1] != 0x01 or ((stream[0] << 8) | stream[1]) % 31:
        raise AssertionError(f'{path.name}: zlib header')
    p, raw, final, blocks = 2, bytearray(), False, 0
    while not final:
        header = stream[p]
        final = bool(header & 1)
        if header & ~1:
            raise AssertionError(f'{path.name}: block type {header >> 1} is not stored')
        length, complement = struct.unpack('<HH', stream[p + 1:p + 5])
        if length ^ complement != 0xFFFF or length == 0:
            raise AssertionError(f'{path.name}: stored block LEN {length} NLEN {complement}')
        raw += stream[p + 5:p + 5 + length]
        p += 5 + length
        blocks += 1
    adler = struct.unpack('>I', stream[p:p + 4])[0]
    if p + 4 != len(stream):
        raise AssertionError(f'{path.name}: bytes after the Adler-32')
    if adler != zlib.adler32(bytes(raw)) & 0xFFFFFFFF:
        raise AssertionError(f'{path.name}: Adler-32')
    if zlib.decompress(stream) != bytes(raw):
        raise AssertionError(f'{path.name}: zlib disagrees with the block walk')
    if blocks != len(idats):
        raise AssertionError(f'{path.name}: {blocks} blocks in {len(idats)} IDAT chunks')

    stride = 1 + width * 3 * depth // 8
    if len(raw) != stride * height:
        raise AssertionError(f'{path.name}: {len(raw)} bytes of scanlines for {width}x{height}x{depth}')
    rows = []
    for y in range(height):
        if raw[y * stride] != 0:
            raise AssertionError(f'{path.name}: row {y} filter {raw[y * stride]}')
        rows.append(bytes(raw[y * stride + 1:(y + 1) * stride]))
    text = {}
    for kind, body in chunks:
        if kind == b'tEXt':
            keyword, value = body.split(b'\0', 1)
            text[keyword.decode('latin-1')] = value.decode('latin-1')
    return dict(width=width, height=height, depth=depth, rows=rows, text=text, kinds=kinds, blocks=blocks)


def check_image(out, line):
    name, file_name, fmt, width, height, pitch, offset, kind, srgb, mean, nonfinite = line.split()
    fmt, width, height, pitch, offset = int(fmt), int(width), int(height), int(pitch), int(offset)
    mean, nonfinite, srgb = float(mean), int(nonfinite), srgb == '1'
    source = SOURCES[fmt]
    src = (out / f'{name}.src').read_bytes()
    bpp = BPP[source]
    if pitch % 256 or offset % 512 or len(src) != offset + pitch * height:
        raise AssertionError(f'{name}: the fixture is not laid out like a readback')

    def pixels(y):
        row = src[offset + y * pitch:offset + y * pitch + width * bpp]
        return [row[x * bpp:(x + 1) * bpp] for x in range(width)]

    total, count, bad = 0.0, 0, 0
    if kind in ('png8', 'png16'):
        png = read_png(out / file_name)
        if (png['width'], png['height']) != (width, height) or png['depth'] != (8 if kind == 'png8' else 16):
            raise AssertionError(f'{name}: IHDR says {png["width"]}x{png["height"]}x{png["depth"]}')
        if (b'sRGB' in png['kinds']) != srgb:
            raise AssertionError(f'{name}: sRGB chunk {"missing" if srgb else "unexpected"}')
        if png['text'] != {'Retoque.Provenance': f'record=fixture;case={name}'}:
            raise AssertionError(f'{name}: tEXt {png["text"]}')
        for y in range(height):
            expected = bytearray()
            for px in pixels(y):
                if source == 'rgba8':
                    rgb = [px[0], px[1], px[2]]
                elif source == 'bgra8':
                    rgb = [px[2], px[1], px[0]]
                elif source == 'rgb10a2':
                    v = struct.unpack('<I', px)[0]
                    codes = [(v >> (10 * c)) & 0x3FF for c in range(3)]
                    wide = [(k << 6) | (k >> 4) for k in codes]
                    expected += struct.pack('>HHH', *wide)
                    for w in wide:
                        total += f32(w / 65535.0)
                        count += 1
                    continue
                else:
                    rgb = [quantise8(b) for b in pixel_float_bits(source, px)]
                expected += bytes(rgb)
                for c in rgb:
                    total += f32(c / 255.0)
                    count += 1
            if png['rows'][y] != bytes(expected):
                raise AssertionError(f'{name}: row {y} differs from the source surface')
        if not math.isclose(mean, total / count, rel_tol=1e-9, abs_tol=1e-12) or nonfinite != 0:
            raise AssertionError(f'{name}: mean {mean} against {total / count}')
        return png

    # PFM: the header, then rows bottom to top, float32 little-endian, every value bit for bit.
    data = (out / file_name).read_bytes()
    header = f'PF\n{width} {height}\n-1.0\n'.encode()
    if not data.startswith(header) or len(data) != len(header) + width * height * 12:
        raise AssertionError(f'{name}: PFM header or size')
    body = data[len(header):]
    for y in range(height):
        stored = height - 1 - y
        expected = [b for px in pixels(y) for b in pixel_float_bits(source, px)]
        got = list(struct.unpack_from(f'<{width * 3}I', body, stored * width * 12))
        if got != expected:
            index = next(i for i, (a, b) in enumerate(zip(got, expected)) if a != b)
            raise AssertionError(f'{name}: row {y} value {index}: {got[index]:08X} against {expected[index]:08X}')
        for b in expected:
            if (b & 0x7F800000) == 0x7F800000:
                bad += 1
            else:
                total += bits_to_float(b)
                count += 1
    if nonfinite != bad or not math.isclose(mean, total / count if count else 0.0, rel_tol=1e-9, abs_tol=1e-12):
        raise AssertionError(f'{name}: mean {mean} / {nonfinite} non-finite against {total / count} / {bad}')
    return None


def privacy_patterns():
    # The suite's own list, so a pattern added there applies here too.
    source = (root / 'tests/repo-privacy/run.py').read_text()
    block = source[source.index('PATTERNS = ['):]
    block = block[:block.index('\n]\n') + 3]
    scope = {'re': re}
    exec(block, scope)
    return scope['PATTERNS']


with tempfile.TemporaryDirectory(prefix='nr-capture-') as temp:
    temp = Path(temp)
    binary = temp / 'capture'
    subprocess.run(['g++', '-std=c++20', '-g', '-O1', '-fno-omit-frame-pointer', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'retoque'),
                    str(here / 'test.cpp'), '-o', str(binary)], check=True)
    out = temp / 'out'
    subprocess.run([str(binary), str(out)], check=True)

    # Every half and every small float, against Python's own decoding.
    half = struct.unpack('<65536I', (out / 'half.bin').read_bytes())
    wrong = [h for h in range(65536) if half[h] != half_bits_to_f32_bits(h)]
    if wrong:
        fail(f'HalfToFloat differs for {len(wrong)} halves, first 0x{wrong[0]:04X}')
    for name, count, bits in (('small11.bin', 2048, 6), ('small10.bin', 1024, 5)):
        got = struct.unpack(f'<{count}I', (out / name).read_bytes())
        wrong = [v for v in range(count) if got[v] != small_bits_to_f32_bits(v, bits)]
        if wrong:
            fail(f'{name}: {len(wrong)} values differ, first {wrong[0]}')

    pngs = {}
    images = (out / 'images.txt').read_text().splitlines()
    if len(images) < 18:
        fail(f'only {len(images)} image cases were written')
    for line in images:
        try:
            png = check_image(out, line)
            if png is not None:
                pngs[line.split()[0]] = png
        except AssertionError as error:
            fail(str(error))

    # Block boundaries: 771 rows of 85 filtered bytes are exactly one stored block, 1542 exactly two, 772 one row
    # into a second, and a larger frame several.
    for name, blocks in (('rgba8_block_exact', 1), ('rgba8_block_two', 2), ('rgba8_block_over', 2),
                         ('rgba8_300x200', 3)):
        if name in pngs and pngs[name]['blocks'] != blocks:
            fail(f'{name}: {pngs[name]["blocks"]} stored blocks, expected {blocks}')

    # A second reader, when there is one: absent is a skip, not a failure.
    try:
        from PIL import Image
    except ImportError:
        print('SKIP: Pillow is not installed; the second reader was not run')
    else:
        for line in images:
            name, file_name, *_ = line.split()
            if not file_name.endswith('.png'):
                continue
            try:
                with Image.open(out / file_name) as image:
                    image.load()
                    if image.info.get('Retoque.Provenance') != f'record=fixture;case={name}':
                        fail(f'{name}: Pillow does not see the tEXt chunk')
                    if name in pngs and pngs[name]['depth'] == 8:
                        if image.mode != 'RGB' or image.tobytes() != b''.join(pngs[name]['rows']):
                            fail(f'{name}: Pillow decodes different pixels')
            except Exception as error:  # a reader that refuses the file is the finding
                fail(f'{name}: Pillow cannot read it: {error}')
        if not failures:
            print('PASS: Pillow reads every PNG, pixels and tEXt alike')

    # The record: the line, the sidecar and the PNG's tEXt chunk say the same thing.
    line = (out / 'provenance.txt').read_text()
    record = json.loads((out / 'provenance.json').read_text())
    pairs = dict(item.split('=', 1) for item in line.split(';'))
    if pairs != record:
        fail(f'the line and the sidecar disagree: {pairs} against {record}')
    expected = {'record': 'set twice keeps one entry', 'quote': 'a "quoted" value', 'backslash': 'C:\\games',
                'semicolon': 'a,b,c', 'utf8': '??t??', 'control': 'tab?here?newline?', 'exe': 'Game'}
    for key, value in expected.items():
        if record.get(key) != value:
            fail(f'record {key} is {record.get(key)!r}, expected {value!r}')
    if list(record)[0] != 'record':
        fail('the record keeps the order its keys were set in')
    tagged = read_png(out / 'provenance.png')['text'].get('Retoque.Provenance')
    if tagged != line:
        fail('the PNG tEXt chunk is not the record line')

    # Privacy: no generated text matches the repository's patterns, though the inputs carried a home path, a user
    # profile, a Wine home and a tailnet host.
    generated = {'provenance.txt': line, 'provenance.json': (out / 'provenance.json').read_text(),
                 'provenance.png tEXt': tagged or ''}
    for name, png in pngs.items():
        generated[f'{name} tEXt'] = ' '.join(png['text'].values())
    for label, text in generated.items():
        for pattern_name, pattern in privacy_patterns():
            if pattern.search(text):
                fail(f'{label} matches the privacy pattern "{pattern_name}"')
        if 'someone' in text or 'tail1234' in text:
            fail(f'{label} carries part of an input path')

print('PASS: PNG structure, CRC-32, Adler-32 and stored blocks; every pixel of every PNG and PFM; every half and '
      'small float; the record in three forms; privacy patterns' if not failures else '')

# --- Source guards: the integration -------------------------------------------------------------------------------

nr = root / 'retoque'
capture_h = (nr / 'dlssnr/DlssNr_PairCapture.h').read_text()
capture_cpp = (nr / 'dlssnr/DlssNr_PairCapture.cpp').read_text()
burst_h = (nr / 'dlssnr/DlssNr_Capture.h').read_text()
readback_h = (nr / 'dlssnr/DlssNr_Readback.h').read_text()
renderer = (nr / 'shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
before = len(failures)


def strip_comments(text):
    return re.sub(r'//[^\n]*', '', text)


def body_of(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth, i = 0, opening
    while True:
        depth += {'{': 1, '}': -1}.get(text[i], 0)
        if depth == 0:
            return text[opening:i + 1]
        i += 1


code_h, code_cpp = strip_comments(capture_h), strip_comments(capture_cpp)

# Nothing waits for the GPU or maps a readback on the render thread: the pure half has no blocking call at all, and
# in the backend the only wait is the writer's condition variable and the only Map is the writer's.
for token in ('WaitForSingleObject', 'SetEventOnCompletion', 'Sleep(', 'sleep_for', '.wait(', '.join(', 'Map('):
    if token in code_h:
        fail(f'DlssNr_PairCapture.h calls {token}')
for token in ('WaitForSingleObject', 'SetEventOnCompletion', 'Sleep(', 'sleep_for', '.join('):
    if token in code_cpp:
        fail(f'DlssNr_PairCapture.cpp calls {token}')
writer = body_of(code_cpp, 'void Writer::Run()')
outside = code_cpp.replace(writer, '')
if '.wait(' in outside or '->Map(' in outside or '->Unmap(' in outside:
    fail('DlssNr_PairCapture.cpp waits, maps or unmaps outside the writer thread')
if '.wait(' not in writer or '->Map(' not in writer or '->Unmap(' not in writer:
    fail('the writer thread no longer maps its readbacks; the guard above proves nothing')

# One copy of the readback helpers (comparison-capture.md, section 4.9): the typed footprint fix lives in one header.
for name, text in (('DlssNr_Capture.h', burst_h), ('DlssNr_PairCapture.cpp', capture_cpp)):
    for token in ('D3D12_HEAP_TYPE_READBACK', 'GetCopyableFootprints', 'CopyTextureRegion'):
        if token in strip_comments(text):
            fail(f'{name} has its own {token}: use DlssNr_Readback.h')
if 'D3D12_RESOURCE_STATE_COPY_DEST' not in readback_h or 'TypedForCopy' not in readback_h:
    fail('DlssNr_Readback.h lost the readback heap state or the typed footprint')

# The record never takes a path: every value set comes from something that is not one, and the executable goes in
# only as its sanitised stem.
for match in re.finditer(r'\.Set\("(\w+)",\s*([^;]*)\);', code_cpp):
    key, value = match.group(1), match.group(2)
    if re.search(r'path|Path|directory|wstring|DllPath|ExePath|\.string\(\)', value):
        fail(f'the record key {key} is set from {value.strip()!r}, which may be a path')
if code_cpp.count('ExePath()') != 1 or 'SanitizeExeStem(Util::ExePath().stem().wstring())' not in code_cpp:
    fail('the executable name must reach the capture only through SanitizeExeStem of its stem')
if 'DllPath()' in code_cpp and 'payload.directory = Util::DllPath()' not in code_cpp:
    fail('the DLL directory may only travel beside the record, as the payload directory')

# The pass: the ticket is polled before anything in Dispatch can return, and the copies are recorded after the last
# write to the target, from the frame the pass read and the frame it wrote, in the states they rest in there.
dispatch = renderer[renderer.index('bool DlssNr_Dx12::Dispatch('):renderer.index('\nnamespace DlssNr\n{')]
poll = dispatch.find('DlssNr::PairCapture::Poll();')
if poll < 0 or not dispatch.index('NotePulsePresent(State::Instance().frameCount);') < poll < dispatch.index(
        'return false;'):
    fail('PairCapture::Poll() must come right after NotePulsePresent, before the first return in Dispatch')
site = dispatch.find('if (wrote && DlssNr::PairCapture::Wanted())')
if 'g_capture.isActive() || DlssNr::PairCapture::Wanted()' not in re.sub(r'\s+', ' ', dispatch):
    fail('a frame that may carry the capture must be labelled capture_active in the GPU timing metadata')
if site < 0 or dispatch.count('if (wrote && DlssNr::PairCapture::Wanted())') != 1:
    fail('the record site must be guarded by `wrote && DlssNr::PairCapture::Wanted()`, once')
elif site < dispatch.index('g_nr.stabilizer->Dispatch('):
    fail('the copies must be recorded after the stabilizer, the last write to the target')
else:
    call = re.sub(r'\s+', ' ', dispatch[site:dispatch.index('facts);', site)])
    if ('AtRecordSite(cmdList, device, g_nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, target, '
            'D3D12_RESOURCE_STATE_UNORDERED_ACCESS,') not in call:
        fail('AtRecordSite must take hdrCopy in NON_PIXEL_SHADER_RESOURCE and the target in UNORDERED_ACCESS')
    if 'TimingContract(' not in call:
        fail("the capture's settings must come from TimingContract, the timing contract's own text")
if 'g_capture.record(cmdList, device, g_nr.hdrCopy,' not in dispatch or 'sourceIsTarget ? g_nr.colorCopy' in dispatch:
    fail("the burst's before must be hdrCopy, the frame itself (comparison-capture.md, open question 4)")
metadata = renderer[renderer.index('DlssNr::GpuTiming::Metadata TimingMetadata('):]
if 'TimingContract(cfg, passes,' not in metadata[:metadata.index('\n}\n')]:
    fail('TimingMetadata must build its contract with TimingContract, so the two cannot drift')
trigger = body_of(renderer, 'void CheckCaptureTrigger()')
if 'RequestPairCapture()' not in trigger or '"burst"' not in trigger:
    fail('the trigger file must ask for the pair, and for the raw burst when it says "burst"')
shutdown = body_of(renderer, 'void Shutdown()\n{')
if 'PairCapture::Shutdown();' not in shutdown:
    fail('DlssNr::Shutdown must let the capture release what is proven done')
host = (nr / 'dlssnr/DlssNr_PresentHost.cpp').read_text()
if 'frame.BridgeHost = true;' not in host or 'bool BridgeHost = false;' not in (
        nr / 'shaders/dlssnr/DlssNr_Common.h').read_text():
    fail('the bridge host must label its frames, or its captures read as the D3D12 present hook')

# Every reason the capture can put on the menu's line has a pt-BR entry. nr-localization's drift guard sees only the
# literals the menu passes to Tr itself; these reach it through a variable.
pack = {line.split('\t')[0] for line in (root / 'retoque/translations/optional/pt-BR.lang').read_text(
    encoding='utf-8').splitlines() if line and not line.startswith('#')}
reasons = []
for source in (capture_h, capture_cpp, (nr / 'dlssnr/DlssNr_ImageFile.h').read_text()):
    reasons += re.findall(r'constexpr const char\* k(?:Reason\w*|Refusal\w*|\w+Words) = "((?:\\.|[^"\\])*)"', source)
if len(reasons) < 15:
    fail(f'found only {len(reasons)} reason literals; the scan no longer matches the code')
for reason in reasons:
    if reason not in pack:
        fail(f'the capture reason {reason!r} has no pt-BR entry')

# No file search or file read for a capture on the render thread: the model's facts are found by the writer.
record_site = body_of(code_cpp, 'void AtRecordSite(')
for token in ('FindModel', 'Describe(', 'TimingModelIdentity', 'file_size', 'exists('):
    if token in record_site:
        fail(f'AtRecordSite calls {token}: file work belongs on the writer thread')
if 'NgxInfo::FindModel()' not in body_of(code_cpp, 'const ModelFacts& ModelFactsOnce()'):
    fail("the model's facts must be found on the writer thread (ModelFactsOnce)")
if 'TimingModelIdentity' in dispatch[site:site + 3000]:
    fail('the record site must not read the model file (TimingModelIdentity) on the render thread')

# Built: the translation unit is in both project files, once.
for project in ('OptiScaler.vcxproj', 'OptiScaler.vcxproj.filters'):
    text = (root / 'OptiScaler' / project).read_text(encoding='utf-8-sig')
    if text.count('Include="..\\retoque\\dlssnr\\DlssNr_PairCapture.cpp"') != 1:
        fail(f'{project} must list ..\\retoque\\dlssnr\\DlssNr_PairCapture.cpp exactly once')

if len(failures) == before:
    print('PASS: source guards -- no wait or map on the render thread, one copy of the readback helpers, no path in '
          'the record, poll and record sites in place, project registration')

if failures:
    for message in failures:
        print(f'FAIL: {message}')
    sys.exit(1)
