#!/usr/bin/env python3
"""
wifilog.py - PC-side companion for 01_wifi_scanner.ino

  log       read the ESP32's serial output and save every AP sample to CSV
            (anything you type while it runs is sent to the board, e.g. "m 2")
  channels  per-channel occupancy from an ALL-mode log
  fit       fit the log-distance path-loss model to a TRACK-mode log whose
            marks are distances in metres

Examples
  pip install pyserial matplotlib
  python wifilog.py log COM5                       # Windows
  python wifilog.py log /dev/ttyUSB0 --track "MyHomeWiFi"
  python wifilog.py channels wifi_20261114_1830.csv
  python wifilog.py fit wifi_20261114_1830.csv --plot
"""
import argparse
import csv
import datetime as dt
import math
import statistics as st
import sys
import threading
import time
from collections import Counter, defaultdict
from pathlib import Path

FIELDS = ["host_time", "scan_id", "esp_ms", "channel", "rssi",
          "bssid", "auth", "ssid", "mark"]


# ----------------------------------------------------------------- parsing
def parse_line(line):
    """Classify one line of firmware output.
    Returns ("AP", dict) | ("SCAN", dict) | ("MARK", label) | ("TEXT", line)."""
    if line.startswith("AP,"):
        parts = line.split(",", 7)          # SSID is last and may contain commas
        if len(parts) == 8:
            _, scan_id, esp_ms, ch, rssi, bssid, auth, ssid = parts
            try:
                return "AP", dict(scan_id=int(scan_id), esp_ms=int(esp_ms),
                                  channel=int(ch), rssi=int(rssi),
                                  bssid=bssid, auth=auth, ssid=ssid)
            except ValueError:
                pass
    elif line.startswith("SCAN,"):
        parts = line.split(",")
        if len(parts) == 5:
            try:
                _, scan_id, esp_ms, n, scan_ms = parts
                return "SCAN", dict(scan_id=int(scan_id), esp_ms=int(esp_ms),
                                    n=int(n), scan_ms=int(scan_ms))
            except ValueError:
                pass
    elif line.startswith("MARK,"):
        parts = line.split(",", 2)
        return "MARK", (parts[2].strip() if len(parts) == 3 else "")
    return "TEXT", line


# ----------------------------------------------------------------- log
def send(ser, text):
    ser.write((text.strip() + "\n").encode())


def cmd_log(args):
    try:
        import serial  # pyserial
    except ImportError:
        sys.exit("pyserial is missing:  pip install pyserial")

    out = Path(args.out or f"wifi_{dt.datetime.now():%Y%m%d_%H%M%S}.csv")
    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    print(f"[opened {args.port} @ {args.baud}; board may reset, waiting...]")
    time.sleep(2.5)
    ser.reset_input_buffer()
    send(ser, "f csv")
    if args.track:
        send(ser, f"t {args.track}")

    def forward_stdin():
        for typed in sys.stdin:
            if typed.strip():
                send(ser, typed)

    threading.Thread(target=forward_stdin, daemon=True).start()
    print(f"[logging to {out}]  type commands (e.g. 'm 2', 'a', 't <ssid>'), Ctrl+C to stop")

    mark, rows, last_full_scan = "", 0, None
    with out.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        try:
            while True:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", "replace").strip()
                kind, data = parse_line(line)
                if kind == "AP":
                    data.update(host_time=dt.datetime.now().isoformat(timespec="milliseconds"),
                                mark=mark)
                    w.writerow(data)
                    f.flush()
                    rows += 1
                    if data["scan_id"] != last_full_scan:      # TRACK sample
                        bar = "#" * max(0, min(35, (data["rssi"] + 100) // 2))
                        print(f"{data['rssi']:5d} dBm  ch{data['channel']:<2} "
                              f"[{mark or '-'}] |{bar}")
                elif kind == "SCAN":
                    last_full_scan = data["scan_id"]
                    print(f"scan {data['scan_id']}: {data['n']} APs in {data['scan_ms']} ms")
                elif kind == "MARK":
                    mark = data
                    print(f"---- mark '{mark}' ----")
                elif line:
                    print(line)
        except KeyboardInterrupt:
            pass
    ser.close()
    print(f"\n[saved {rows} samples to {out}]")


# ----------------------------------------------------------------- analysis
def load_rows(path):
    with open(path, newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        r["rssi"] = int(r["rssi"])
        r["channel"] = int(r["channel"])
    return rows


def cmd_channels(args):
    rows = load_rows(args.csv)
    # strongest RSSI seen per BSSID, and its channel
    best = {}
    for r in rows:
        b = r["bssid"]
        if b not in best or r["rssi"] > best[b]["rssi"]:
            best[b] = r
    per_ch = defaultdict(list)
    for r in best.values():
        per_ch[r["channel"]].append(r)

    print(f"{len(best)} unique BSSIDs across {len({r['scan_id'] for r in rows})} scans\n")
    print("ch  APs  strongest  overlap-load*")
    for c in range(1, 14):
        aps = per_ch.get(c, [])
        strongest = max((a["rssi"] for a in aps), default=None)
        # 20 MHz channels are 5 MHz apart, so APs within 4 channels overlap.
        # Weight each by how close (in channels) and how strong it is.
        load = sum((1 - abs(c - a["channel"]) / 5) * 10 ** (a["rssi"] / 10)
                   for a in best.values() if abs(c - a["channel"]) < 5)
        load_dbm = 10 * math.log10(load) if load > 0 else float("-inf")
        s = f"{strongest:4d} dBm" if strongest is not None else "    -   "
        print(f"{c:2d}  {len(aps):3d}  {s}   {load_dbm:7.1f} dBm  {'#' * len(aps)}")
    print("\n* rough interference estimate: summed power (mW) of APs on overlapping "
          "channels, weighted by channel separation. Lower is better.")


def linfit(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    slope = sxy / sxx
    icpt = my - slope * mx
    ss_res = sum((y - (icpt + slope * x)) ** 2 for x, y in zip(xs, ys))
    ss_tot = sum((y - my) ** 2 for y in ys)
    r2 = 1 - ss_res / ss_tot if ss_tot else 1.0
    return slope, icpt, r2, math.sqrt(ss_res / n)


def cmd_fit(args):
    rows = load_rows(args.csv)
    if args.ssid:
        rows = [r for r in rows if r["ssid"] == args.ssid]
    if args.bssid:
        rows = [r for r in rows if r["bssid"].upper() == args.bssid.upper()]
    else:  # default: the BSSID with the most samples (the tracked one)
        if not rows:
            sys.exit("no rows after filtering")
        top = Counter(r["bssid"] for r in rows).most_common(1)[0][0]
        rows = [r for r in rows if r["bssid"] == top]

    by_d = defaultdict(list)
    for r in rows:
        try:
            d = float(r["mark"].lower().rstrip("m"))
        except ValueError:
            continue
        if d > 0:
            by_d[d].append(r["rssi"])

    if len(by_d) < 2:
        sys.exit("need samples at 2+ distances (use marks like 'm 1', 'm 2', 'm 4' ...)")

    print(f"AP {rows[0]['ssid']}  {rows[0]['bssid']}  ch {rows[0]['channel']}\n")
    print("  d (m)   n   mean dBm   std   min  max")
    ds = sorted(by_d)
    for d in ds:
        v = by_d[d]
        sd = st.pstdev(v) if len(v) > 1 else 0.0
        print(f"{d:7.2f} {len(v):4d}   {st.mean(v):7.1f}  {sd:5.1f}  {min(v):4d} {max(v):4d}")

    # log-distance model: RSSI(d) = RSSI(1 m) - 10 n log10(d)
    # Fit on every sample (not just means) so each distance is weighted by its sample count.
    xs = [10 * math.log10(d) for d in ds for _ in by_d[d]]
    ys = [rssi for d in ds for rssi in by_d[d]]
    slope, icpt, r2, sigma = linfit(xs, ys)
    n_exp = -slope
    print(f"\nRSSI(d) = {icpt:.1f} dBm - 10 x {n_exp:.2f} x log10(d)")
    print(f"  path-loss exponent n = {n_exp:.2f}   (free space = 2.0; indoors typically 2.5-4)")
    print(f"  RSSI at 1 m          = {icpt:.1f} dBm")
    print(f"  R^2 = {r2:.3f},  shadowing sigma = {sigma:.1f} dB")
    print(f"  -> range estimate where RSSI = -80 dBm: "
          f"{10 ** ((icpt + 80) / (10 * n_exp)):.1f} m")

    if args.plot:
        import matplotlib.pyplot as plt
        all_d = [d for d in ds for _ in by_d[d]]
        plt.scatter(all_d, ys, s=8, alpha=0.3, label="samples")
        plt.plot(ds, [st.mean(by_d[d]) for d in ds], "o", label="mean per distance")
        fd = [ds[0] * (ds[-1] / ds[0]) ** (i / 100) for i in range(101)]
        plt.plot(fd, [icpt - 10 * n_exp * math.log10(d) for d in fd],
                 label=f"fit: n = {n_exp:.2f}")
        plt.plot(fd, [icpt - 20 * math.log10(d) for d in fd], "--",
                 label="free space (n = 2)")
        plt.xscale("log")
        from matplotlib.ticker import FuncFormatter
        fmt = FuncFormatter(lambda v, _: f"{v:g}")
        plt.gca().xaxis.set_major_formatter(fmt)
        plt.gca().xaxis.set_minor_formatter(fmt)
        plt.xlabel("distance (m)")
        plt.ylabel("RSSI (dBm)")
        plt.title(f"Path loss - {rows[0]['ssid']} ch {rows[0]['channel']}")
        plt.grid(True, which="both", alpha=0.3)
        plt.legend()
        out = Path(args.csv).with_suffix(".png")
        plt.savefig(out, dpi=150, bbox_inches="tight")
        print(f"\nplot saved to {out}")
        plt.show()


# ----------------------------------------------------------------- main
def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    pl = sub.add_parser("log", help="log serial output to CSV")
    pl.add_argument("port", help="e.g. COM5 or /dev/ttyUSB0")
    pl.add_argument("--baud", type=int, default=115200)
    pl.add_argument("--out", help="CSV path (default: wifi_<timestamp>.csv)")
    pl.add_argument("--track", help="start in TRACK mode on this SSID")
    pl.set_defaults(func=cmd_log)

    pc = sub.add_parser("channels", help="channel occupancy from an ALL-mode log")
    pc.add_argument("csv")
    pc.set_defaults(func=cmd_channels)

    pf = sub.add_parser("fit", help="path-loss fit from a TRACK-mode log")
    pf.add_argument("csv")
    pf.add_argument("--ssid")
    pf.add_argument("--bssid")
    pf.add_argument("--plot", action="store_true")
    pf.set_defaults(func=cmd_fit)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
