#!/usr/bin/env python3
"""Generate synthetic test frames and produce ground truth reference frames using lsfg-vk-cli debug.

Generates:
1. Translation test frames:
   - 0.dds: High-frequency procedurally textured frame
   - 1.dds: Frame translated by (+8, +4) pixels
2. Rotation & zoom test frames:
   - 0.dds: Centered procedurally textured frame
   - 1.dds: Frame rotated by +2.0 degrees and zoomed by 1.02x about image center (960, 540)

Can also invoke `lsfg-vk-cli debug` to generate the ground-truth intermediate frames into `ref/`.

Usage:
    # Generate input test frames and ground truth reference frames
    python3 tools/lsfg-study/make_test_frames.py --run-cli

    # Custom output directory
    python3 tools/lsfg-study/make_test_frames.py --out-dir /path/to/Tmp/lsfg-study --run-cli
"""

import argparse
import struct
import subprocess
import sys
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw


def make_dds_header(width: int, height: int) -> bytes:
    """Create a standard 128-byte uncompressed RGBA8 (32bpp) DDS header."""
    magic = b"DDS "
    size = 124
    flags = 0x100F  # DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT
    pitch = width * 4
    depth = 0
    mips = 1
    reserved1 = b"\x00" * 44
    pf_size = 32
    pf_flags = 0x41  # DDPF_RGB | DDPF_ALPHAPIXELS
    fourcc = 0
    rgb_bits = 32
    r_mask = 0x000000FF
    g_mask = 0x0000FF00
    b_mask = 0x00FF0000
    a_mask = 0xFF000000
    caps = 0x1000  # DDSCAPS_TEXTURE
    caps2 = 0
    caps3 = 0
    caps4 = 0
    reserved2 = 0

    header = struct.pack(
        "<4sIIIIIII44sIIIIIIIIIIIII",
        magic,
        size,
        flags,
        height,
        width,
        pitch,
        depth,
        mips,
        reserved1,
        pf_size,
        pf_flags,
        fourcc,
        rgb_bits,
        r_mask,
        g_mask,
        b_mask,
        a_mask,
        caps,
        caps2,
        caps3,
        caps4,
        reserved2,
    )
    assert len(header) == 128
    return header


def save_image_pair(img: Image.Image, dds_path: Path, png_path: Path):
    """Save an image as both uncompressed RGBA8 DDS and PNG."""
    dds_path.parent.mkdir(parents=True, exist_ok=True)
    header = make_dds_header(img.width, img.height)
    raw = img.convert("RGBA").tobytes()
    with open(dds_path, "wb") as f:
        f.write(header)
        f.write(raw)
    img.save(png_path)


def generate_base_scene(width: int = 1920, height: int = 1080) -> Image.Image:
    """Generate a high-frequency procedural texture rich with tracking features."""
    img = Image.new("RGBA", (width, height), (25, 28, 36, 255))
    draw = ImageDraw.Draw(img)

    # 1. Background checkerboard grid
    grid = 40
    for y in range(0, height, grid):
        for x in range(0, width, grid):
            if (x // grid + y // grid) % 2 == 0:
                draw.rectangle([x, y, x + grid, y + grid], fill=(45, 52, 65, 255))

    # 2. Diagonal stripes
    for i in range(-height, width, 60):
        draw.line([(i, 0), (i + height, height)], fill=(70, 80, 95, 180), width=8)

    # 3. Fine high-frequency grid in central area
    cx, cy = width // 2, height // 2
    for y in range(cy - 200, cy + 200, 10):
        draw.line([(cx - 300, y), (cx + 300, y)], fill=(120, 130, 150, 100), width=1)
    for x in range(cx - 300, cx + 300, 10):
        draw.line([(x, cy - 200), (x, cy + 200)], fill=(120, 130, 150, 100), width=1)

    # 4. Geometric colored landmarks
    np.random.seed(42)
    for i in range(28):
        px = int(120 + (width - 240) * (i / 27.0))
        py = int(120 + (height - 240) * ((i * 7 + 3) % 23 / 22.0))
        r = 30 + (i % 5) * 12
        color = ((50 + i * 8) % 256, (120 + i * 11) % 256, (200 - i * 6) % 256, 255)
        if i % 3 == 0:
            draw.ellipse([px - r, py - r, px + r, py + r], fill=color, outline=(255, 255, 255, 255), width=3)
        elif i % 3 == 1:
            draw.rectangle([px - r, py - r, px + r, py + r], fill=color, outline=(0, 0, 0, 255), width=3)
        else:
            points = [(px, py - r), (px + r, py + r), (px - r, py + r)]
            draw.polygon(points, fill=color, outline=(255, 255, 0, 255))

    return img


def generate_translation_frames(base: Image.Image, out_dir: Path, dx: int = 8, dy: int = 4):
    """Generate translation pair: frame 0 and frame 1 translated by (dx, dy)."""
    trans_dir = out_dir / "input_translation"
    save_image_pair(base, trans_dir / "0.dds", trans_dir / "0.png")

    w, h = base.size
    trans_img = Image.new("RGBA", (w, h), (25, 28, 36, 255))
    trans_img.paste(base, (dx, dy))
    save_image_pair(trans_img, trans_dir / "1.dds", trans_dir / "1.png")
    print(f"Generated translation frames in: {trans_dir}")


def generate_rotzoom_frames(base: Image.Image, out_dir: Path, deg: float = 2.0, zoom: float = 1.02):
    """Generate rotation & zoom pair: frame 0 and frame 1 rotated and zoomed about center."""
    rot_dir = out_dir / "input_rotzoom"
    save_image_pair(base, rot_dir / "0.dds", rot_dir / "0.png")

    w, h = base.size
    rotated = base.rotate(deg, resample=Image.BICUBIC, center=(w // 2, h // 2))
    w_z, h_z = int(w * zoom), int(h * zoom)
    scaled = rotated.resize((w_z, h_z), resample=Image.BICUBIC)
    left = (w_z - w) // 2
    top = (h_z - h) // 2
    cropped = scaled.crop((left, top, left + w, top + h))

    save_image_pair(cropped, rot_dir / "1.dds", rot_dir / "1.png")
    print(f"Generated rotzoom frames in: {rot_dir}")


def run_lsfg_vk_cli(out_dir: Path, width: int = 1920, height: int = 1080):
    """Run lsfg-vk-cli debug to generate reference frames."""
    ref_dir = out_dir / "ref"
    for test_name in ["translation", "rotzoom"]:
        input_dir = out_dir / f"input_{test_name}"
        target_ref = ref_dir / test_name
        target_ref.mkdir(parents=True, exist_ok=True)
        cmd = [
            "lsfg-vk-cli",
            "debug",
            "-w", str(width),
            "-h", str(height),
            "-m", "2",
            "-f", "1.0",
            "-o", str(target_ref),
            str(input_dir),
        ]
        print(f"Running: {' '.join(cmd)}")
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            print(f"Error running lsfg-vk-cli: {res.stderr}", file=sys.stderr)
            sys.exit(res.returncode)

        # Convert generated DDS to PNG for convenience
        gen_dds = target_ref / "1.dds"
        if gen_dds.exists():
            img = Image.open(gen_dds)
            img.save(target_ref / "1.png")
            print(f"Successfully generated reference: {gen_dds} ({target_ref / '1.png'})")


def main():
    parser = argparse.ArgumentParser(description="Generate synthetic test frames for LSFG study")
    default_out = Path(__file__).resolve().parents[2] / "Tmp" / "lsfg-study"
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=default_out,
        help="Root output directory (default: Tmp/lsfg-study)",
    )
    parser.add_argument("--width", type=int, default=1920, help="Frame width (default: 1920)")
    parser.add_argument("--height", type=int, default=1080, help="Frame height (default: 1080)")
    parser.add_argument("--run-cli", action="store_true", help="Run lsfg-vk-cli debug to produce ground truth")
    args = parser.parse_args()

    base = generate_base_scene(args.width, args.height)
    generate_translation_frames(base, args.out_dir)
    generate_rotzoom_frames(base, args.out_dir)

    if args.run_cli:
        run_lsfg_vk_cli(args.out_dir, args.width, args.height)


if __name__ == "__main__":
    main()
