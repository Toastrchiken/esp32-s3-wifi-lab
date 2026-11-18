# 01 — Wi-Fi Scanner & RSSI Path-Loss Logger (ESP32-S3)

Firmware that turns an ESP32-S3 into a 2.4 GHz Wi-Fi survey tool, plus a Python
tool that logs its measurements and fits an indoor **log-distance path-loss model**.

Part of a series of ESP32-S3 Wi-Fi/networking projects:
**01 scanner/RSSI** → 02 AP+STA modes → 03 web dashboard → 04 UDP vs TCP throughput → 05 ESP-NOW range → 06 MQTT telemetry.

![path-loss plot](images/path_loss.png)
<!-- TODO: replace with your plot from `wifilog.py fit ... --plot` -->

## Features
- **Survey mode:** active scan of channels 1–13 (AU regulatory domain) listing every
  access point's channel, RSSI, BSSID, security type and SSID, plus per-channel occupancy.
- **Track mode:** locks onto a single BSSID and samples its RSSI at ~6 Hz on its channel
  only, with a moving average, min and max.
- **Distance markers:** tag samples with the measurement distance from the PC (`m 2` = 2 m).
- **PC tooling (`wifilog.py`):** logs to CSV, estimates channel congestion, and fits
  path-loss exponent *n*, the 1 m reference RSSI and shadowing σ, with a plot.

## Hardware
| Item | Notes |
|---|---|
| ESP32-S3-DevKitC-1 v1.0 | Programmed via the UART port (CP2102N bridge) |
| Micro-USB data cable | |
| Home router (2.4 GHz) | Device under test for track mode |

## How it works
**Scanning.** In an active scan the ESP32 visits each channel in turn, sends a probe
request, and listens for ~120 ms for beacons and probe responses. Each response carries
the AP's SSID, BSSID, channel and security type, and the radio measures its received
power (RSSI, in dBm). One sweep of 13 channels takes ~1.6 s.

**Tracking.** Track mode first finds the strongest AP with the requested SSID, then scans
only that AP's channel with an SSID + BSSID filter. A single-channel scan takes
~150 ms, so RSSI can be sampled fast enough to watch fading and obstruction effects
live. Locking onto the BSSID matters because one SSID is often served by several
radios (mesh nodes, dual-band routers).

**Data pipeline.**
```
ESP32 firmware --CSV lines over UART--> CP2102N --USB--> PC: wifilog.py --> .csv --> fit/plot
             <------------- commands typed at the PC ("t <ssid>", "m 2") <-------------
```

**Path-loss model.** Received power falls off with distance *d* as

&nbsp;&nbsp;&nbsp;&nbsp;RSSI(d) = RSSI(1 m) − 10·n·log₁₀(d) + X<sub>σ</sub>

where *n* is the path-loss exponent (2 in free space, typically 2.5–4 indoors) and
X<sub>σ</sub> is shadowing/multipath variation. `wifilog.py fit` estimates *n*,
RSSI(1 m) and σ by least squares over every sample (not only the per-distance means).

## Usage
Build and flash with PlatformIO (official `espressif32` platform, Arduino core 2.0.17):
```
pio run -e uart -t upload        # or the → button in VS Code
```
If upload reports *wrong boot mode*: hold **BOOT**, tap **RST**, release **BOOT**, then upload.

Serial commands (115 200 baud): `a` survey · `t <ssid>` track · `m <label>` marker ·
`r` reset stats · `i <ms>` interval · `f csv|table` format · `p` pause · `h` help.

PC side:
```
pip install pyserial matplotlib
python wifilog.py log COM4 --track "MyWiFi"   # type "m 1", "m 2", ... at each distance
python wifilog.py fit wifi_<timestamp>.csv --plot
python wifilog.py channels wifi_<timestamp>.csv
```

## Results
<!-- TODO: fill in from your own runs. Keep the raw CSVs in data/. -->

**Setup:** [date], [location, e.g. hallway, line of sight to router], router on channel
[x], board held at [height] with the body out of the line of sight.

### Path loss vs distance
| Distance (m) | Samples | Mean RSSI (dBm) | Std (dB) |
|---|---|---|---|
| 1 | | | |
| 2 | | | |
| 4 | | | |
| 8 | | | |
| 16 | | | |

Fitted model: RSSI(d) = [ ] dBm − 10 × **[n]** × log₁₀(d), R² = [ ], σ = [ ] dB.
Estimated range to −80 dBm: [ ] m.

[1–2 sentences: how *n* compares with free space (n = 2) and why — walls, furniture, people.]

### Obstruction loss
| Condition | RSSI (dBm) | Loss (dB) |
|---|---|---|
| Baseline (clear line of sight) | −52 to −55 | — |
| [Body between board and router] | −60 to −65 | ≈ 9 |
| [Laptop screen / wall / hand over antenna] | | |

### Channel occupancy
[Paste the `wifilog.py channels` output or a summary: how many APs on 1/6/11, the least
congested channel, and whether any networks sit on overlapping channels.]

## What I learned
- Active vs passive scanning, beacons, and the SSID vs BSSID distinction.
- dBm as a log power scale (+3 dB = 2×, +10 dB = 10×); a −60 dBm reading from a
  +20 dBm router is 80 dB (10⁸×) of path loss.
- Multipath fading causes a few dB of variation at a fixed position and changes over
  distances of about half a wavelength (~6 cm at 2.4 GHz), so measurements need averaging.
- The human body (mostly water) absorbs 2.4 GHz strongly: ~[x] dB measured.
- Only channels 1, 6 and 11 are non-overlapping at 2.4 GHz (20 MHz channels, 5 MHz spacing).
- [Anything that went wrong and how you fixed it, e.g. auto-reset into download mode,
  Arduino core 2.x vs 3.x API differences.]

## Possible extensions
- Colour the onboard RGB LED (GPIO48) by RSSI for walk-around surveys.
- Average in linear (mW) rather than dB, and compare the two.
- Repeat with the board's antenna rotated to measure its radiation pattern.
