# ESP32-S3 Wi-Fi Lab

A progression of hands-on projects on the **ESP32-S3** exploring 2.4 GHz Wi-Fi, RF
measurement and IP networking, from raw radio measurements up to application-layer
telemetry. Each project has its own folder with source, a write-up and **measured
results**.

| # | Project | Focus | Status |
|---|---|---|---|
| 01 | [Wi-Fi scanner & RSSI path-loss logger](01_wifi_scanner/) | Active scanning, RSSI, channel occupancy, log-distance path-loss fit | ✅ Working, results in progress |
| 02 | [Ultrasonic radar security scanner](02_ultrasonic_radar/) | Servo-swept HC-SR04, clutter map + M-of-N detection, phone dashboard over HTTP, AP/STA modes, push alerts | 🔧 Built and simulation-tested, hardware testing next |
| 03 | UDP vs TCP throughput test | Transport-layer trade-offs, packet loss, latency | Planned |
| 04 | ESP-NOW range test | Connectionless peer-to-peer link, range vs RSSI vs packet loss | Planned |
| 05 | MQTT telemetry node | Publish/subscribe, brokers, low-power reporting | Planned |

## Hardware
- ESP32-S3-DevKitC-1 v1.0 (dual-core Xtensa LX7, 2.4 GHz 802.11 b/g/n + BLE 5)
- Micro-USB data cable; home router as the test access point
- Project 02: HC-SR04 ultrasonic sensor, SG90 servo, 1 kΩ/2 kΩ divider

## Tooling
- **Firmware:** C++ with the Arduino framework on PlatformIO (`espressif32` platform)
- **Analysis:** Python (pyserial, matplotlib) for logging, curve fitting and plots

## Building any project
```
cd 01_wifi_scanner
pio run -e uart -t upload
pio device monitor
```
See each project's README for wiring, commands and results.
