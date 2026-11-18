# 02 — Wi-Fi Ultrasonic Radar Security Scanner (ESP32-S3)

A scanning range sensor that maps a room, learns what "empty" looks like, and alerts
your phone when something new appears. An HC-SR04 ultrasonic sensor rides on a servo
sweeping 0–180°; the ESP32-S3 serves a live radar dashboard over Wi-Fi.

It's sonar rather than radar, but the signal processing is radar technique:
sweep scanning, a clutter map, background subtraction and M-of-N detection.

<!-- TODO: add a photo of the build and a screenshot of the phone dashboard -->
<!-- ![build](images/build.jpg) ![dashboard](images/dashboard.png) -->

## Features
- **Live radar display** on your phone: range vs bearing, with returns fading over time
- **Manual aim:** point the sensor from a slider on the web page
- **Learn room:** averages 3 sweeps into a per-bearing baseline (clutter map)
- **Arm:** flags anything closer than the baseline by more than a threshold; an alarm
  needs **2 of 3 pings** at a bearing to agree, which suppresses false alarms
- **Alerts:** red banner, vibration and an optional beep on the phone; one event per target
  (repeats within ±15° / 15 s are grouped)
- **Event log** with real timestamps (NTP, or the phone's clock in AP mode); CSV export
- **Push notifications** to the ntfy app when on home Wi-Fi (optional)
- **Two Wi-Fi modes:** joins your router (STA), or falls back to its own access point (AP)
- No external libraries: only what ships with the Arduino-ESP32 core

## Hardware
| Part | Notes |
|---|---|
| ESP32-S3-DevKitC-1 | |
| HC-SR04 ultrasonic sensor | 5 V part: **its ECHO pin needs a voltage divider** (below) |
| SG90 micro servo | Small enough to run from the board's 5 V pin |
| Resistors: 1 kΩ + 2 kΩ | Echo divider (1 k + 2.2 k or 10 k + 20 k also fine) |
| 100–470 µF capacitor | Optional but recommended across 5 V/GND to stop resets when the servo moves |
| Breadboard + jumper wires | |

## Wiring
```
ESP32-S3-DevKitC-1                     HC-SR04
   5V0 ────────────┬──────────────────── VCC
                   │
   GND ──────────┬─┼──────────────────── GND
                 │ │
 GPIO5 ──────────┼─┼──────────────────── TRIG     (3.3 V trigger is fine)
                 │ │
 GPIO6 ──┬───────┼─┼──[ 1 kΩ ]────────── ECHO     (5 V output!)
         │       │ │
       [2 kΩ]    │ │
         │       │ │
        GND      │ │                      SG90 servo
                 │ └──────────────────── red    (+5 V)
                 └────────────────────── brown  (GND)
 GPIO4 ───────────────────────────────── orange (signal)

 Optional: 100–470 µF electrolytic across 5V0 and GND, close to the servo (+ to 5V0).
```

**Why the divider:** the HC-SR04 runs on 5 V and its ECHO pin outputs 5 V pulses. ESP32
GPIOs are 3.3 V only and are **not 5 V tolerant**. The divider gives 5 V × 2k/(1k+2k) ≈ 3.3 V.
If you have the 3.3 V variant (HC-SR04P or RCWL-1601), power it from 3V3 and skip the divider.

**Power:** an SG90 draws a few hundred mA when moving, which USB can supply. If the board
resets whenever the servo moves, add the capacitor or use a separate 5 V supply for the
servo, with grounds connected. A bigger servo (MG996R) **must** have its own 5–6 V supply.

## Mounting
- Stick the sensor to the servo horn (double-sided tape, hot glue or a rubber band over a
  scrap of cardboard), facing outwards.
- Fix the servo to something heavy (Blu-Tack it to a book) at roughly chest height,
  with the scan arc facing across the room.
- Leave enough slack in the sensor wires for a full 180° turn.
- If 0° and 180° look swapped on the display, tick **Mirror left/right** on the page.

## Setup
1. Copy `include/secrets.example.h` to `include/secrets.h` and add your Wi-Fi name and
   password (2.4 GHz, WPA2-Personal: eduroam won't work). Skip this to use AP mode.
2. Build and upload with PlatformIO (→ in the status bar). If you get *wrong boot mode*:
   hold **BOOT**, tap **RST**, release **BOOT**, upload again.
3. Open the serial monitor (115200). It prints the address to open, e.g.
   `http://192.168.1.42` or `http://esp32-radar.local`.
   - **AP mode:** join Wi-Fi `ESP32-Radar` (password `radar1234`) and open `http://192.168.4.1`.
4. On your phone: **Scan** to see the room. Clear the area, press **Learn room** (~15 s),
   then **Arm**. Walk into the scan arc and the alarm should trigger.

**Push notifications (optional, home Wi-Fi only):** install the *ntfy* app, subscribe to
a random topic name such as `ram-radar-7f3k9q`, put the same name in `NTFY_TOPIC` in
`secrets.h`, and rebuild. Alarms then arrive even when the dashboard is closed.

## How it works
**Ranging.** The ESP32 sends a 10 µs trigger, the HC-SR04 emits a 40 kHz ultrasonic burst,
and the ECHO pin stays high for the round-trip time *t*. Range = *t* × *c* / 2, where
the speed of sound *c* = 331.3 + 0.606·T m/s (T in °C), so air temperature is a setting.
Each bearing gets several pings, 50 ms apart so old echoes die out, and the median is used.

**Scanning.** A non-blocking state machine steps the servo (default 5°), waits for it to
settle, pings, then moves on, bouncing between 0° and 180°. One sweep takes about 5 s.

**Clutter map.** *Learn room* records 3 sweeps and stores the median range at each bearing.
Bearings with no echo (open doorways, anything beyond ~4 m) are stored as "far".

**Detection.** When armed, each bearing gets 3 pings. A ping is *intrusive* if it is closer
than the baseline by more than max(threshold, 10% of baseline). **2 of 3** intrusive pings
confirm a target (M-of-N detection), so single noisy pings or dropouts don't cause alarms.
Alerts within ±15° and 15 s of a previous one are grouped so one person gives one event.

**Networking.** The ESP32 runs an HTTP server on port 80. The page polls
`/api/poll?p=<seq>&e=<seq>` about 3× per second and gets only the new scan points and
events since its last request, using sequence numbers so nothing is missed or repeated.
| Endpoint | Purpose |
|---|---|
| `GET /` | Dashboard (`web/index.html`, embedded at build time) |
| `GET /api/poll?p=&e=` | Status + new points + new events |
| `GET /api/baseline` | Learned range per bearing (cm, `null` = unmeasured) |
| `GET /api/cmd?c=scan\|stop\|aim&angle=\|learn\|arm\|disarm\|ack\|release\|set&...` | Control |
| `GET /api/time?ms=` | Phone sends its clock for log timestamps |

### Radar concepts used
| Here | In radar |
|---|---|
| Time of flight × speed of sound / 2 | Same equation with *c* = speed of light |
| ~15° ultrasonic beam limits how close two objects can be and still be separated | Antenna beamwidth sets angular resolution |
| Learned per-bearing baseline | Clutter map |
| "Closer than baseline by > threshold" | Background subtraction / detection threshold |
| 2 of 3 pings must agree | M-of-N detection logic |
| Soft or angled surfaces return weak or no echoes | Radar cross-section, specular reflection |

## Testing
`sim/sim.cpp` is a **software-in-the-loop test**: it compiles the real firmware on a PC
against mock Arduino libraries and a simulated room (walls at 3 m, a cupboard at 1.5 m,
an open doorway, ±3 cm noise, 3% dropped echoes), then walks a simulated person in:
```
g++ -std=gnu++17 -I sim/mocks -I src -DESP_ARDUINO_VERSION_MAJOR=2 sim/sim.cpp -o sim_test && ./sim_test
```
Result: room learned in 15.6 s, **0 false alarms** in 30 s armed, **1 event** for a person
standing at 120° for 30 s, and a new event when a second person appears at 35°.

## Results
<!-- TODO: fill in from your own measurements; keep CSVs in data/ and plots in images/ -->

**Range accuracy** (sensor pointed at a wall, tape-measured distance):
| True (cm) | Measured mean (cm) | Error (cm) |
|---|---|---|
| 20 | | |
| 50 | | |
| 100 | | |
| 200 | | |
| 300 | | |

**Room map:** `python plot_scan.py data/radar_scan_<time>.csv` → `images/room_map.png`

**Detection:** [false alarms over N minutes empty; walk-throughs detected at 1 / 2 / 3 m;
threshold used; what was missed and why, e.g. clothing absorbing sound, angled approach]

**Angular resolution:** [two bottles at the same range: smallest angle at which they still
show as separate returns, compared with the ~15° beamwidth]

## What I learned
- [e.g. why the median of several pings beats a single reading]
- [e.g. how the threshold trades missed detections against false alarms]
- [e.g. why soft materials and angled surfaces are hard to detect, and the radar equivalent]
- [e.g. level-shifting 5 V signals for 3.3 V logic]

## Extensions
- Add a buzzer or a relay output for a physical alarm
- Store events in flash (LittleFS) so they survive a reboot
- Replace the HC-SR04 with a VL53L1X time-of-flight laser sensor (narrower beam) and compare
- Use a 24 GHz mmWave radar module (e.g. HLK-LD2410) for real radar and compare detection
