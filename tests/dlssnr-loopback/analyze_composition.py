#!/usr/bin/env python3
"""Read GPU PPM captures, never the montage. No fitted alignment or image modification."""
import hashlib
import json
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

# (ReversibleMode, TransferStrength, ColourStrength, Style), in the order the harness runs them.
# mode0, composed, repeat and style0 are deliberately the SAME configuration: they are the control.
# If their numbers disagree, the machine is the variable and nothing else in the table is readable.
POINTS = dict(mode0=(0, 1, 1, 0), mode1=(1, 1, 1, 0), mode3=(3, 1, 1, 0), mode4=(4, 1, 1, 0),
              composed=(0, 1, 1, 0), direct=(2, 1, 1, 0), t0=(0, 0, 1, 0), t25=(0, .25, 1, 0),
              t50=(0, .5, 1, 0), t75=(0, .75, 1, 0), c0=(0, 1, 0, 0), c25=(0, 1, .25, 0),
              c50=(0, 1, .5, 0), c75=(0, 1, .75, 0), c125=(0, 1, 1.25, 0), c150=(0, 1, 1.5, 0),
              repeat=(0, 1, 1, 0), style0=(0, 1, 1, 0), style1=(0, 1, 1, 1), style2=(0, 1, 1, 2))


def saturation(rgb):
    maximum = rgb.max(axis=-1)
    return np.divide(maximum - rgb.min(axis=-1), maximum, out=np.zeros_like(maximum), where=maximum > 0)


def linear(rgb):
    return np.where(rgb <= .04045, rgb / 12.92, ((rgb + .055) / 1.055) ** 2.4)


def stats(a, b):
    delta = np.abs(b - a) * 255
    return {'mae': float(delta.mean()), 'max': int(round(delta.max())),
            'saturation_before': float(saturation(a).mean()), 'saturation_after': float(saturation(b).mean()),
            'linear_Y_before': float((linear(a) @ [.2126, .7152, .0722]).mean()),
            'linear_Y_after': float((linear(b) @ [.2126, .7152, .0722]).mean()),
            'encoded_Yprime_before': float((a @ [.2126, .7152, .0722]).mean()),
            'encoded_Yprime_after': float((b @ [.2126, .7152, .0722]).mean())}


def gamma_fit(a, b):
    result = []
    for channel in range(3):
        x, y = a[..., channel].ravel(), b[..., channel].ravel()
        valid = (x > .02) & (x < .98) & (y > .02) & (y < .98)
        x, y = x[valid], y[valid]
        lx, ly = np.log(x), np.log(y)
        # Fixed gain=1, offset=0; least squares in log space, not a free gain/gamma fit.
        exponent = float(np.sum(lx * ly) / np.sum(lx * lx))
        result.append({'gamma': exponent, 'samples': len(x),
                       'rgb_rmse': float(np.sqrt(np.mean((x ** exponent - y) ** 2)))})
    return result


def main(directory):
    root = Path(directory)
    hashes, records, comparisons = {}, {}, {}
    log = (root / 'dlssnr-loopback.log').read_text(errors='replace')
    observed = re.findall(
        r'^COMPOSITION-POINT name=(\w+) mode=(\d+) transfer=(\d+\.\d+) colour=(\d+\.\d+) style=(\d+)$', log, re.M)
    actual = [(name, (int(mode), float(t), float(c), int(st))) for name, mode, t, c, st in observed]
    if actual != list(POINTS.items()):
        raise RuntimeError('missing, duplicated or unexpected composition point settings in anchored log')

    def read(point, generation, frame, side):
        w, h = (960, 540) if generation == 2 else (1280, 720)
        name = f'{point}-g{generation}-f{frame}-{side}.ppm'
        raw = (root / name).read_bytes()
        header = f'P6\n{w} {h}\n255\n'.encode()
        if not raw.startswith(header) or len(raw) != len(header) + w * h * 3:
            raise RuntimeError(f'incomplete/invalid {name}')
        hashes[name] = hashlib.sha256(raw).hexdigest()
        return np.frombuffer(raw[len(header):], np.uint8).reshape(h, w, 3)

    for gen in (1, 2, 3):
        for frame in (0, 1, 15):
            clean = read('composed', gen, frame, 'before')
            a = clean.astype(np.float64) / 255
            h, w = a.shape[:2]
            band = np.zeros((h, w), dtype=bool)
            band[:h//8, :w//2] = True
            masks = {'bar_band': band, 'saturated_bars': band & (saturation(a) >= .5),
                     'neutral': saturation(a) <= .05, 'rest_of_scene': ~band}
            outputs = {}
            for point in POINTS:
                before, after = read(point, gen, frame, 'before'), read(point, gen, frame, 'after')
                if not np.array_equal(before, clean):
                    raise RuntimeError(f'source mismatch {point} g{gen} f{frame}')
                if frame == 0 and not np.array_equal(before, after):
                    raise RuntimeError(f'ApplyModel=0 control not exact: {point} g{gen}')
                outputs[point] = after
                b = after.astype(np.float64) / 255
                entry = {'full': stats(a, b), 'gamma': gamma_fit(a, b)}
                for name, mask in masks.items():
                    entry[name] = stats(a[mask], b[mask])
                records[f'{point}-g{gen}-f{frame}'] = entry
            for label, p, q in (('direct_vs_composed', 'direct', 'composed'),
                                 ('repeat_vs_composed', 'repeat', 'composed')):
                delta = np.abs(outputs[p].astype(np.int16) - outputs[q].astype(np.int16))
                comparisons[f'{label}-g{gen}-f{frame}'] = {
                    'mae': float(delta.mean()), 'max': int(delta.max()), 'changed_channels': int(np.count_nonzero(delta))}
    report = {'points_mode_transfer_colour_style': POINTS,
              'definitions': {'saturation': 'mean HSV S of encoded RGB, black S=0',
                              'linear_Y': 'mean BT.709 Y after piecewise sRGB decoding',
                              'encoded_Yprime': 'mean dot(encoded RGB, BT.709 weights); NOT physical luminance',
                              'gamma': 'y=x^gamma, fixed gain/offset; least squares in log space; x,y in (.02,.98)',
                              'deltas': 'absolute RGB byte difference; no registration, resized PNG or colour transform',
                              'bar_band': 'x < width/2, y < height/8; includes neutral bar patches',
                              'saturated_bars': 'bar_band AND source HSV S >= .5',
                              'neutral': 'source HSV S <= .05; rest_of_scene is a separate, coloured region'},
              'records': records, 'comparisons': comparisons, 'sha256_ppm': hashes}
    (root / 'composition-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('point       Sat delta%  linearY delta% gamma RGB        bars MAE  neutral MAE (g3 f15)')
    for point in POINTS:
        r = records[f'{point}-g3-f15']; full = r['full']
        print(f"{point:10s} {100*(full['saturation_after']/full['saturation_before']-1):9.4f} "
              f"{100*(full['linear_Y_after']/full['linear_Y_before']-1):13.4f} "
              + '/'.join(f"{v['gamma']:.4f}" for v in r['gamma'])
              + f" {r['bar_band']['mae']:9.4f} {r['neutral']['mae']:11.4f}")
    print('direct / repeat comparisons:', json.dumps(comparisons))
    # These are for inspection only: every metric above came from full-resolution readbacks.
    canvas = Image.new('RGB', (1440, 800))
    draw = ImageDraw.Draw(canvas)
    for i, (point, side, label) in enumerate((('composed', 'before', 'SOURCE'),
                                             ('composed', 'after', 'COMPOSED'), ('direct', 'after', 'DIRECT MODE 2'))):
        im = Image.fromarray(read(point, 3, 15, side)); x = i * 480
        draw.text((x+8, 8), label, fill='white')
        canvas.paste(im.resize((480, 270)), (x, 30))
        canvas.paste(im.crop((0, 0, 480, 90)), (x, 330))
        canvas.paste(im.crop((320, 300, 800, 640)), (x, 450))
    canvas.save(root / 'composition-vs-direct.png')


if __name__ == '__main__':
    main(sys.argv[1])
