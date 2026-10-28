#!/usr/bin/env python3
# tools/diff_heatmap.py - Visual Authenticity & Heatmap Diff Validator
#
# Compares reference DOOM frame against offline baked (.WL + .FLT) Saturn VDP1 simulation:
#   - Green       : 100% pixel match / sub-perceptual difference
#   - Yellow      : Minor lighting / colormap rounding delta (acceptable)
#   - Red/Magenta : Texture misalignment, UV drift, seam crack, or missing geometry (FATAL)

import os
import sys
import argparse
import numpy as np
from PIL import Image
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.colors as mcolors

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from tools.saturn_sim.map_data import MapData
from tools.saturn_sim.rasterizer import Rasterizer
from tools.saturn_sim.renderer import SaturnRenderer

def render_pose(map_data, x, y, z, ang, use_baked_walls=True, mode="perspective"):
    rasterizer = Rasterizer(320, 224)
    rasterizer.mode = mode
    renderer = SaturnRenderer(map_data, rasterizer)
    renderer.set_camera(x, y, z, ang)

    # Temporarily toggle baked_walls if needed
    orig_bw = map_data.baked_walls
    if not use_baked_walls:
        map_data.baked_walls = None

    renderer.render_frame()

    map_data.baked_walls = orig_bw
    return rasterizer.fb.copy()

def compute_heatmap(ref_img, test_img):
    """
    Computes per-pixel delta and generates RGB heatmap:
      - Green   : Delta == 0
      - Yellow  : 0 < Delta <= 35 (minor colormap/shading rounding)
      - Orange  : 35 < Delta <= 75 (noticeable variance)
      - Red     : Delta > 75 (seam / misalignment / fatal geometry defect)
    """
    assert ref_img.shape == test_img.shape, f"Image shape mismatch: {ref_img.shape} vs {test_img.shape}"
    diff = np.sqrt(np.sum((ref_img.astype(np.float32) - test_img.astype(np.float32)) ** 2, axis=2))

    h, w = diff.shape
    heatmap = np.zeros((h, w, 3), dtype=np.uint8)

    # Perfect match: Emerald Green
    mask_perf = (diff == 0)
    heatmap[mask_perf] = [0, 220, 40]

    # Sub-perceptual match (diff <= 15): Bright Green
    mask_close = (diff > 0) & (diff <= 20)
    heatmap[mask_close] = [80, 240, 50]

    # Minor lighting / quantization (20 < diff <= 45): Yellow-Green to Yellow
    mask_minor = (diff > 20) & (diff <= 45)
    t_minor = (diff[mask_minor] - 20) / 25.0
    heatmap[mask_minor, 0] = (200 + 55 * t_minor).astype(np.uint8)
    heatmap[mask_minor, 1] = (240 - 20 * t_minor).astype(np.uint8)
    heatmap[mask_minor, 2] = 20

    # Moderate delta (45 < diff <= 80): Orange
    mask_mod = (diff > 45) & (diff <= 80)
    heatmap[mask_mod] = [255, 140, 0]

    # Fatal mismatch (diff > 80): Bright Crimson Red
    mask_fatal = (diff > 80)
    heatmap[mask_fatal] = [255, 0, 40]

    total_pixels = h * w
    pct_perfect = np.sum(diff == 0) / total_pixels * 100.0
    pct_acceptable = np.sum(diff <= 45) / total_pixels * 100.0
    pct_fatal = np.sum(diff > 80) / total_pixels * 100.0
    avg_delta = np.mean(diff)
    max_delta = np.max(diff)

    stats = {
        "pct_perfect": pct_perfect,
        "pct_acceptable": pct_acceptable,
        "pct_fatal": pct_fatal,
        "avg_delta": avg_delta,
        "max_delta": max_delta,
        "fatal_count": int(np.sum(mask_fatal))
    }

    return heatmap, diff, stats

def main():
    parser = argparse.ArgumentParser(description="DOOM Saturn Visual Authenticity Heatmap Validator")
    parser.add_argument("--wad", default="cd/DOOM.WAD", help="Path to WAD file")
    parser.add_argument("--map", default="E1M1", help="Map name")
    parser.add_argument("--x", type=float, default=1056.0, help="Camera X")
    parser.add_argument("--y", type=float, default=-3616.0, help="Camera Y")
    parser.add_argument("--z", type=float, default=41.0, help="Camera Z")
    parser.add_argument("--ang", type=float, default=90.0, help="Camera Angle")
    parser.add_argument("--ref", default=None, help="Optional external reference PNG (320x224)")
    parser.add_argument("--test", default=None, help="Optional external test PNG (320x224)")
    parser.add_argument("--mode", default="perspective", choices=["perspective", "affine"], help="Rasterizer mode")
    parser.add_argument("--out", default="tools/diff_heatmap.png", help="Output heatmap path")
    args = parser.parse_args()

    print(f"=== DOOM Saturn Visual Authenticity Heatmap Test ({args.map}) ===")

    if args.ref and args.test:
        ref_img = np.array(Image.open(args.ref).convert("RGB"))
        test_img = np.array(Image.open(args.test).convert("RGB"))
    else:
        print(f"Loading map {args.map} from {args.wad}...")
        map_data = MapData(wad_path=args.wad, map_name=args.map, flt_dir="cd", wl_dir="cd")

        if args.ref:
            ref_img = np.array(Image.open(args.ref).convert("RGB"))
        else:
            print("Rendering Golden Reference (Vanilla Unbaked Carmack Math)...")
            ref_img = render_pose(map_data, args.x, args.y, args.z, args.ang,
                                  use_baked_walls=False, mode=args.mode)

        if args.test:
            test_img = np.array(Image.open(args.test).convert("RGB"))
        else:
            print("Rendering Test (Offline Baked .WL + .FLT Zero-Runtime-Math Pipeline)...")
            test_img = render_pose(map_data, args.x, args.y, args.z, args.ang,
                                   use_baked_walls=True, mode=args.mode)

    # Resize if needed to match dimensions
    if ref_img.shape != test_img.shape:
        print(f"Resizing test image from {test_img.shape[:2]} to match reference {ref_img.shape[:2]}...")
        pil_test = Image.fromarray(test_img).resize((ref_img.shape[1], ref_img.shape[0]), Image.Resampling.BILINEAR)
        test_img = np.array(pil_test)

    heatmap, diff_vals, stats = compute_heatmap(ref_img, test_img)

    # Plot 3-panel comparison figure
    fig, axes = plt.subplots(1, 3, figsize=(18, 6), dpi=150)

    axes[0].imshow(ref_img)
    axes[0].set_title("Carmack Golden Reference\n(Vanilla Unbaked DOOM Math)", fontsize=11, fontweight="bold")
    axes[0].axis("off")

    axes[1].imshow(test_img)
    axes[1].set_title("Master Blueprint Saturn Pipeline\n(Pre-Baked .WL + .FLT Streaming)", fontsize=11, fontweight="bold")
    axes[1].axis("off")

    axes[2].imshow(heatmap)
    axes[2].set_title(f"Visual Authenticity Heatmap\n(Green=Match, Yellow=Shading, Red=Fatal)", fontsize=11, fontweight="bold")
    axes[2].axis("off")

    # Legend / Status bar
    verdict = "PASS (Watertight & Authentic)" if stats["pct_acceptable"] >= 98.0 and stats["pct_fatal"] < 1.0 else "FAIL"
    status_text = (
        f"Pose: X={args.x}, Y={args.y}, Z={args.z}, Ang={args.ang} | "
        f"Match Rate: {stats['pct_acceptable']:.1f}% | "
        f"Perfect Match: {stats['pct_perfect']:.1f}% | "
        f"Avg Delta: {stats['avg_delta']:.2f} | "
        f"Fatal Pixels: {stats['fatal_count']} ({stats['pct_fatal']:.2f}%) | Verdict: {verdict}"
    )

    fig.text(0.5, 0.03, status_text, ha="center", fontsize=10, fontweight="bold",
             bbox=dict(boxstyle="round,pad=0.5", facecolor="#e8f5e9" if verdict.startswith("PASS") else "#ffebee",
                       edgecolor="#4caf50" if verdict.startswith("PASS") else "#f44336", lw=1.5))

    plt.tight_layout(rect=[0, 0.08, 1, 1])
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    plt.savefig(args.out)
    plt.close()

    print(f"\n--- Visual Authenticity Heatmap Audit ---")
    print(f"Map:             {args.map}")
    print(f"Camera Pose:     X={args.x}, Y={args.y}, Z={args.z}, Ang={args.ang} deg")
    print(f"Acceptable Rate: {stats['pct_acceptable']:.2f}% (Green + Yellow)")
    print(f"Perfect Match:   {stats['pct_perfect']:.2f}% (Pure Green)")
    print(f"Fatal Delta:     {stats['pct_fatal']:.2f}% (Red/Magenta)")
    print(f"Average Delta:   {stats['avg_delta']:.2f} RGB Euclidean distance")
    print(f"Verdict:         {verdict}")
    print(f"Heatmap image:   {args.out}\n")

if __name__ == "__main__":
    main()
