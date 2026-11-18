#!/usr/bin/env python3
"""
plot_scan.py - polar plot of a radar scan exported from the phone dashboard ("Scan CSV").

  python plot_scan.py radar_scan_2026-11-15-16-30-00.csv
  python plot_scan.py radar_scan_....csv --max-range 300 --out room_map.png

Green = normal returns, orange = candidate (1 ping closer than baseline),
red = confirmed intrusion (2 of 3 pings), dashed blue = learned room baseline.
"""
import argparse
import csv
import math
from collections import defaultdict

import matplotlib.pyplot as plt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--max-range", type=float, default=None, help="cm (default: auto)")
    ap.add_argument("--out", help="output PNG (default: next to the CSV)")
    args = ap.parse_args()

    rows = []
    with open(args.csv, newline="") as f:
        for r in csv.DictReader(f):
            rows.append((float(r["bearing_deg"]), float(r["range_cm"]),
                         float(r["baseline_cm"]), int(r["flag"])))
    if not rows:
        raise SystemExit("empty CSV")

    base = {}
    for a, _, b, _ in rows:
        if b >= 0:
            base[a] = b
    valid = [d for _, d, _, _ in rows if d >= 0]
    rmax = args.max_range or max(valid + list(base.values()) + [100]) * 1.05

    fig = plt.figure(figsize=(8, 4.8))
    ax = fig.add_subplot(projection="polar")
    ax.set_thetamin(0)
    ax.set_thetamax(180)
    ax.set_rlim(0, rmax)
    step = 50 if rmax <= 250 else 100                      # 4-8 range rings
    ticks = [t for t in range(step, int(rmax) + 1, step)]
    ax.set_rticks(ticks)
    ax.set_yticklabels([f"{t / 100:g} m" for t in ticks], fontsize=8)

    colours = {0: "#22aa55", 1: "#ff9900", 2: "#e02020"}
    labels = {0: "return", 1: "candidate", 2: "confirmed intrusion"}
    by_flag = defaultdict(list)
    for a, d, _, fl in rows:
        if d >= 0:
            by_flag[fl].append((math.radians(a), min(d, rmax)))
    for fl in (0, 1, 2):
        if by_flag[fl]:
            th, r = zip(*by_flag[fl])
            ax.scatter(th, r, s=10 if fl == 0 else 28, c=colours[fl],
                       alpha=0.35 if fl == 0 else 0.9, label=labels[fl])
    if base:
        angs = sorted(base)
        ax.plot([math.radians(a) for a in angs], [min(base[a], rmax) for a in angs],
                "--", color="#3388ff", lw=1.5, label="room baseline")

    ax.set_title("Ultrasonic radar scan", pad=12)
    ax.legend(loc="upper center", bbox_to_anchor=(0.5, 0.12), ncol=4, fontsize=8, frameon=False)

    hits = by_flag[2]
    if hits:
        print(f"{len(hits)} confirmed intrusion returns, bearings "
              f"{min(math.degrees(t) for t, _ in hits):.0f}-{max(math.degrees(t) for t, _ in hits):.0f} deg")
    out = args.out or args.csv.rsplit(".", 1)[0] + ".png"
    fig.savefig(out, dpi=150, bbox_inches="tight")
    print(f"saved {out}")


if __name__ == "__main__":
    main()
