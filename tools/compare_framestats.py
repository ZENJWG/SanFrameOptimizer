#!/usr/bin/env python3
"""Compare FrameStats runs from SanFrameOptimizer log files (stdlib only)."""
import argparse
import re
from pathlib import Path
from statistics import median

PATTERN = re.compile(r"FrameStats:.*?\bsamples=(\d+).*?\bavg=([\d.]+) ms.*?\bp95=([\d.]+) ms.*?\bp99=([\d.]+) ms")
def read(path):
    samples = []
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        match = PATTERN.search(line)
        if match:
            count, avg, p95, p99 = map(float, match.groups())
            if count > 0 and avg > 0 and p95 > 0 and p99 > 0:
                samples.append((count, avg, p95, p99))
    return samples

def report(path):
    frames = read(path)
    if not frames:
        raise ValueError(f"No valid FrameStats windows in {path}")
    total = sum(x[0] for x in frames)
    weighted_avg = sum(x[0] * x[1] for x in frames) / total
    # Window percentiles cannot be combined into an exact full-session percentile.
    return {"windows": len(frames), "frames": int(total),
            "estimated_fps": 1000.0 / weighted_avg,
            "median_window_p95_ms": median(x[2] for x in frames),
            "median_window_p99_ms": median(x[3] for x in frames)}

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("baseline")
    ap.add_argument("candidate")
    args = ap.parse_args()
    a, b = report(args.baseline), report(args.candidate)
    for k in a:
        print(f"{k}: baseline={a[k]:.3f}, candidate={b[k]:.3f}, delta={b[k]-a[k]:+.3f}")
    print("Compare equivalent in-game routes/settings; window percentiles are not pooled.")

if __name__ == "__main__":
    main()
