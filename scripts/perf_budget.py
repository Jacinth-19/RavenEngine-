#!/usr/bin/env python3
"""RavenEngine performance budget calculator.

Implements the frame-time model from docs/PERFORMANCE.md and prints the predicted
gpu/cpu time for a given target resolution, frame rate and quality tier, plus a
pass/fail verdict per sub-budget. Pure math, no engine required.

Examples:
    scripts/perf_budget.py --target 1080p --fps 60 --tier medium --gpu igpu
    scripts/perf_budget.py --target 1440p --fps 120 --tier high --gpu mid
    scripts/perf_budget.py --target 1080p --fps 60 --tier all
"""

import argparse
import math
import sys


# ---- model constants (docs/PERFORMANCE.md §3) -------------------------------------------
TARGETS = {"1080p": (1920, 1080), "1440p": (2560, 1440)}
TIERS = {
    "low": dict(resolution_scale=0.5, shadow=512, msaa=1, particles=512, overdraw=1.4,
                bytes_per_px=70, flop_per_px=25, draw_calls=150, features=dict(bloom=0, ssao=0, taa=0, ssr=0)),
    "medium": dict(resolution_scale=0.6666667, shadow=1024, msaa=2, particles=1024, overdraw=1.8,
                   bytes_per_px=95, flop_per_px=45, draw_calls=300, features=dict(bloom=1, ssao=0, taa=0, ssr=0)),
    "high": dict(resolution_scale=1.0, shadow=2048, msaa=4, particles=4096, overdraw=2.2,
                 bytes_per_px=150, flop_per_px=70, draw_calls=600, features=dict(bloom=1, ssao=1, taa=1, ssr=0)),
    "ultra": dict(resolution_scale=1.0, shadow=4096, msaa=8, particles=8192, overdraw=2.4,
                  bytes_per_px=190, flop_per_px=90, draw_calls=1000, features=dict(bloom=1, ssao=1, taa=1, ssr=1)),
}
GPU_CLASSES = {
    "igpu": dict(label="integrated (Iris Xe-class)", gflops=1200.0, bandwidth_gbs=35.0, efficiency=0.30),
    "mid":  dict(label="mid discrete (RTX 4060/RX 7600-class)", gflops=8000.0, bandwidth_gbs=200.0, efficiency=0.50),
    "high": dict(label="enthusiast (RX 7900 XTX/RTX 4090-class)", gflops=40000.0, bandwidth_gbs=700.0, efficiency=0.60),
}
FEATURE_COST = {"bloom": 0.06, "ssao": 0.12, "taa": 0.08, "ssr": 0.30}   # fraction of t_forward each
MSAA_ROP_FACTOR = {1: 1.0, 2: 1.25, 4: 1.6, 8: 2.2}
MSAA_SHADE_FACTOR = {1: 1.0, 2: 1.1, 4: 1.25, 8: 1.5}                    # per-sample-ish shading cost


def internal_extent(target, scale):
    return max(1, math.floor(target * scale))


def budget_for(target, fps):
    total = 1000.0 / fps
    return total, total * 0.85  # 15% headroom for CPU submit / vsync / compositor


def evaluate(target_name, fps, tier_name, gpu_name):
    tw, th = TARGETS[target_name]
    tier = TIERS[tier_name]
    gpu = GPU_CLASSES[gpu_name]

    iw, ih = internal_extent(tw, tier["resolution_scale"]), internal_extent(th, tier["resolution_scale"])
    px = iw * ih
    msaa = tier["msaa"]

    bytes_frame = px * tier["bytes_per_px"] * tier["overdraw"] * MSAA_ROP_FACTOR[msaa]
    t_mem = bytes_frame / (gpu["bandwidth_gbs"] * 1e9) * 1e3

    eff = gpu["gflops"] * 1e9 * gpu["efficiency"]
    t_alu = px * tier["overdraw"] * tier["flop_per_px"] * MSAA_SHADE_FACTOR[msaa] / eff * 1e3

    t_shadow = tier["shadow"] ** 2 * 8.0 / eff * 1e3                      # cheap depth-only pass
    t_upscale = (tw * th - px) * 30.0 / eff * 1e3 if px < tw * th else 0.0  # bilinear-ish resolve of the delta
    t_forward = max(t_mem, t_alu) + t_shadow                            # memory or ALU bound, additive shadow
    for feat, frac in FEATURE_COST.items():
        if tier["features"][feat]:
            t_forward += frac * t_forward
    t_cpu = tier["draw_calls"] * 5e-3                                      # ~5 µs per draw call submitted

    total_gpu = t_forward + t_upscale
    frame_budget, usable = budget_for(target_name, fps)
    verdict_gpu = "PASS" if total_gpu <= usable else ("TIGHT" if total_gpu <= frame_budget else "FAIL")
    verdict_cpu = "PASS" if t_cpu <= usable * 0.5 else "TIGHT"
    fps_estimate = 1000.0 / max(total_gpu + t_cpu, 1e-6)

    return {
        "target": target_name, "fps_target": fps, "tier": tier_name, "gpu": gpu_name,
        "internal_resolution": f"{iw}x{ih}", "internal_pixels": px,
        "pixels_vs_native": round(px / (tw * th), 4),
        "gpu_ms": round(total_gpu, 2), "cpu_submit_ms": round(t_cpu, 2),
        "frame_budget_ms": round(frame_budget, 2), "usable_budget_ms": round(usable, 2),
        "estimated_fps_vsync_off": round(fps_estimate, 1),
        "gpu_verdict": verdict_gpu, "cpu_verdict": verdict_cpu,
        "bandwidth_gbs_used": round(bytes_frame * fps / 1e9, 1),
    }


def print_report(r):
    print(f"\n=== {r['target']} @ {r['fps_target']} Hz — tier {r['tier'].upper()} — {r['gpu']} class ===")
    print(f"  internal resolution : {r['internal_resolution']}  ({r['pixels_vs_native']*100:.1f}% of native pixels)")
    print(f"  GPU frame time      : {r['gpu_ms']:.2f} ms   [{r['gpu_verdict']}]")
    print(f"  CPU submit estimate : {r['cpu_submit_ms']:.2f} ms   [{r['cpu_verdict']}]")
    print(f"  frame budget        : {r['frame_budget_ms']:.2f} ms ({r['usable_budget_ms']:.2f} ms usable after 15% headroom)")
    print(f"  estimated vsync-off : ~{r['estimated_fps_vsync_off']:.0f} fps")


def main():
    parser = argparse.ArgumentParser(description="RavenEngine frame-time budget model")
    parser.add_argument("--target", choices=[*TARGETS.keys(), "all"], default="1080p")
    parser.add_argument("--fps", type=int, choices=[60, 120], default=60)
    parser.add_argument("--tier", choices=[*TIERS.keys(), "all"], default="medium")
    parser.add_argument("--gpu", choices=[*GPU_CLASSES.keys()], default="igpu")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    args = parser.parse_args()

    targets = list(TARGETS) if args.target == "all" else [args.target]
    tiers = list(TIERS) if args.tier == "all" else [args.tier]
    reports = [evaluate(t, args.fps, q, args.gpu) for t in targets for q in tiers]

    if args.json:
        import json
        print(json.dumps(reports, indent=2))
        return 0

    for r in reports:
        print_report(r)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
