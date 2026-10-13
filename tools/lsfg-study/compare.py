#!/usr/bin/env python3
"""Image comparison tool for LSFG frame generation study.

Compares two images (DDS, PNG, JPEG, etc.) and reports:
- PSNR (Peak Signal-to-Noise Ratio) in dB
- Maximum Absolute Difference (per channel and overall)
- Mean Absolute Error (MAE)
- Percentage of pixels exceeding a given absolute difference threshold
- Optional visual diff export (amplified difference heatmap)

Usage examples:
    # Basic comparison (default threshold = 2/255)
    python3 tools/lsfg-study/compare.py frame_ref.dds frame_d3d12.dds

    # Specify threshold (0-255 scale or 0.0-1.0 float scale)
    python3 tools/lsfg-study/compare.py frame_ref.png frame_d3d12.png --threshold 5

    # Compare RGB only, ignore Alpha
    python3 tools/lsfg-study/compare.py frame_ref.dds frame_d3d12.dds --rgb-only

    # Save visual amplified diff heatmap
    python3 tools/lsfg-study/compare.py ref.dds d3d12.dds --save-diff diff.png --diff-amp 10
"""

import argparse
import sys
from pathlib import Path
import numpy as np
from PIL import Image


def load_image(path: Path, rgb_only: bool = True) -> np.ndarray:
    """Load an image and return as float32 numpy array in range [0, 255]."""
    if not path.is_file():
        raise FileNotFoundError(f"Image file not found: {path}")

    img = Image.open(path)
    if rgb_only:
        img = img.convert("RGB")
    else:
        img = img.convert("RGBA")

    return np.array(img, dtype=np.float32)


def compute_metrics(
    img_a: np.ndarray,
    img_b: np.ndarray,
    threshold: float,
) -> dict:
    """Compute comparison metrics between two images of identical dimensions."""
    if img_a.shape != img_b.shape:
        raise ValueError(
            f"Image dimensions do not match: {img_a.shape} vs {img_b.shape}"
        )

    diff = np.abs(img_a - img_b)
    mse = np.mean((img_a - img_b) ** 2)

    if mse == 0.0:
        psnr = float("inf")
    else:
        psnr = float(10.0 * np.log10(255.0**2 / mse))

    max_abs_diff = float(np.max(diff))
    mae = float(np.mean(diff))

    # Pixel-level difference: maximum difference across color channels for each pixel
    if diff.ndim == 3:
        pixel_max_diff = np.max(diff, axis=-1)
    else:
        pixel_max_diff = diff

    pixels_above = np.sum(pixel_max_diff > threshold)
    total_pixels = pixel_max_diff.size
    pct_above = float(pixels_above / total_pixels * 100.0)

    # Per-channel max difference
    if diff.ndim == 3:
        per_channel_max = [float(np.max(diff[..., c])) for c in range(diff.shape[-1])]
    else:
        per_channel_max = [max_abs_diff]

    return {
        "psnr": psnr,
        "mse": float(mse),
        "mae": mae,
        "max_abs_diff": max_abs_diff,
        "per_channel_max": per_channel_max,
        "pixels_above_threshold": int(pixels_above),
        "total_pixels": int(total_pixels),
        "pct_above_threshold": pct_above,
        "threshold": threshold,
    }


def save_diff_heatmap(
    img_a: np.ndarray,
    img_b: np.ndarray,
    out_path: Path,
    amp: float = 10.0,
):
    """Save an amplified absolute difference heatmap."""
    diff = np.abs(img_a - img_b)
    amplified = np.clip(diff * amp, 0.0, 255.0).astype(np.uint8)

    if amplified.shape[-1] == 4:
        # Keep alpha opaque for visualization
        amplified[..., 3] = 255

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_img = Image.fromarray(amplified)
    out_img.save(out_path)


def main():
    parser = argparse.ArgumentParser(
        description="Compare two images (PSNR, max abs diff, % over threshold)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("img1", type=Path, help="Path to first image (reference)")
    parser.add_argument("img2", type=Path, help="Path to second image (candidate)")
    parser.add_argument(
        "-t",
        "--threshold",
        type=float,
        default=2.0,
        help="Absolute difference threshold in [0, 255] (default: 2.0, or 2/255 levels)",
    )
    parser.add_argument(
        "--include-alpha",
        action="store_true",
        help="Include alpha channel in comparison (default: RGB only)",
    )
    parser.add_argument(
        "--save-diff",
        type=Path,
        default=None,
        help="Optional path to write visual amplified difference image",
    )
    parser.add_argument(
        "--diff-amp",
        type=float,
        default=10.0,
        help="Amplification factor for --save-diff image (default: 10.0)",
    )
    parser.add_argument(
        "--max-pct-fail",
        type=float,
        default=None,
        help="Exit with error if percentage of pixels above threshold exceeds this value",
    )

    args = parser.parse_args()

    # Support threshold given in [0, 1] range:
    threshold = args.threshold
    if 0.0 < threshold <= 1.0:
        threshold = threshold * 255.0

    try:
        rgb_only = not args.include_alpha
        img_a = load_image(args.img1, rgb_only=rgb_only)
        img_b = load_image(args.img2, rgb_only=rgb_only)
        metrics = compute_metrics(img_a, img_b, threshold)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    ch_names = ["R", "G", "B"] if rgb_only else ["R", "G", "B", "A"]

    print("==================================================")
    print("                IMAGE COMPARISON REPORT           ")
    print("==================================================")
    print(f"Image 1 (Ref) : {args.img1}")
    print(f"Image 2 (Test): {args.img2}")
    print(f"Dimensions    : {img_a.shape[1]}x{img_a.shape[0]} ({', '.join(ch_names)})")
    print("--------------------------------------------------")
    if np.isinf(metrics["psnr"]):
        print("PSNR          : Identical (inf dB)")
    else:
        print(f"PSNR          : {metrics['psnr']:.3f} dB")
    print(f"MSE           : {metrics['mse']:.4f}")
    print(f"MAE           : {metrics['mae']:.4f} / 255 ({metrics['mae']/255.0*100:.3f}%)")
    print(f"Max Abs Diff  : {metrics['max_abs_diff']:.1f} / 255")
    ch_max_str = ", ".join(f"{c}: {m:.1f}" for c, m in zip(ch_names, metrics["per_channel_max"]))
    print(f"  Per channel : {ch_max_str}")
    print(f"Threshold     : > {metrics['threshold']:.2f} / 255")
    print(
        f"Over Threshold: {metrics['pct_above_threshold']:.3f}% "
        f"({metrics['pixels_above_threshold']:,} / {metrics['total_pixels']:,} pixels)"
    )
    print("==================================================")

    if args.save_diff:
        save_diff_heatmap(img_a, img_b, args.save_diff, amp=args.diff_amp)
        print(f"Saved difference heatmap to: {args.save_diff} (amplified x{args.diff_amp})")

    if args.max_pct_fail is not None and metrics["pct_above_threshold"] > args.max_pct_fail:
        print(
            f"FAILED: {metrics['pct_above_threshold']:.2f}% pixels over threshold "
            f"(allowed: <= {args.max_pct_fail:.2f}%)",
            file=sys.stderr,
        )
        return 2

    return 0


if __name__ == "__main__":
    sys.exit(main())
