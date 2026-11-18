// Copy this file to include/secrets.h and fill it in.
// secrets.h is git-ignored, so your Wi-Fi password never goes to GitHub.
// Without secrets.h the ESP32 starts its own network "ESP32-Radar" (password radar1234).
#pragma once

#define WIFI_SSID "YourHomeWiFi"     // 2.4 GHz network; eduroam/enterprise Wi-Fi won't work
#define WIFI_PASS "your-password"

// Optional phone push notifications via https://ntfy.sh (home Wi-Fi only):
// install the ntfy app, subscribe to a hard-to-guess topic, and put the same name here.
// Anyone who knows the topic name can read the alerts, so make it random.
#define NTFY_TOPIC ""                 // e.g. "ram-radar-7f3k9q"
