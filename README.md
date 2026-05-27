# ☀️ Solar NerdMiner with MQTT & Home Assistant

A solar-powered Bitcoin miner running [NerdMinerV2](https://github.com/BitMaker-hub/NerdMiner_v2) on an ESP32-C3 SuperMini, with MQTT publishing and Home Assistant auto-discovery. Mining is automatically scheduled around civil twilight times for your location, fetched daily from the sunrise-sunset.org API.

![Mining Active](https://img.shields.io/badge/Mining-Active-brightgreen) ![HA Auto-Discovery](https://img.shields.io/badge/Home%20Assistant-Auto%20Discovery-blue) ![Solar Powered](https://img.shields.io/badge/Powered-Solar-yellow)

---

## 📷 What it does

- ⛏️ Mines Bitcoin via Stratum protocol using NerdMinerV2
- 📡 Publishes live stats to MQTT every 60 seconds
- 🏠 Auto-discovers sensors in Home Assistant — no config files needed
- 🌅 Mines only within the civil twilight window for your location
- ⏰ Handles UK GMT/BST clock changes automatically
- 📅 Fetches fresh sunrise/sunset times daily at 00:05 local time
- 🔋 Designed to run on solar-charged 18650 cells

---

## 🛠️ Hardware

| Component | Details |
|---|---|
| Microcontroller | ESP32-C3 SuperMini |
| Power | 2x 18650 cells in parallel, charged by small solar panel |
| Solar panel | Repurposed from a solar wall light (~105mm x 55mm) |
| Optional | BH1750 lux sensor + BME280 on separate ESP32 for weather monitoring |

---

## 📦 Dependencies

Added to `platformio.ini` alongside existing NerdMinerV2 dependencies:

```ini
knolleary/PubSubClient@^2.8
```

The following are already included in NerdMinerV2:
- `bblanchon/ArduinoJson`
- `arduino-libraries/NTPClient`

---

## ⚙️ Configuration

All configuration is at the top of `src/mqtt_manager.h`:

```cpp
// MQTT broker
#define MQTT_BROKER      "YOUR_MQTT_BROKER_IP"   // e.g. "192.168.1.50"
#define MQTT_PORT        1883
#define MQTT_USER        ""                       // Leave blank if anonymous
#define MQTT_PASS        ""

// Your location (for sunrise/sunset calculation)
#define LOCATION_LAT     "YOUR_LATITUDE"          // e.g. "51.5074"
#define LOCATION_LNG     "YOUR_LONGITUDE"         // e.g. "-0.1278"

// Buffer added to civil twilight window
#define TWILIGHT_BUFFER_SECS  1800     // 30 minutes either side

// How often to publish to MQTT
#define MQTT_INTERVAL_MS  60000UL      // 60 seconds
```

### Finding your coordinates
Go to [Google Maps](https://maps.google.com), right-click your location and copy the coordinates.

### Timezone
The code uses the UK POSIX timezone string `GMT0BST,M3.5.0/1,M10.5.0` which handles GMT/BST automatically. For other timezones, find your POSIX string at [https://github.com/nayarsystems/posix_tz_db](https://github.com/nayarsystems/posix_tz_db).

---

## 📊 MQTT Topics

All topics are published under `nerdminer/solar/`:

| Topic | Description | Example |
|---|---|---|
| `nerdminer/solar/status` | Online/offline (LWT) | `online` |
| `nerdminer/solar/hashrate` | Current hash rate | `28.7 KH/s` |
| `nerdminer/solar/shares` | Completed shares | `42` |
| `nerdminer/solar/total_kh` | Total KH since boot | `4618735` |
| `nerdminer/solar/uptime` | Uptime since boot | `0 05:17:08` |
| `nerdminer/solar/valids` | Valid blocks found | `0` |
| `nerdminer/solar/mining_active` | Currently mining? | `true` |
| `nerdminer/solar/window_start` | Today's mining start | `03:46` |
| `nerdminer/solar/window_end` | Today's mining end | `22:09` |

---

## 🏠 Home Assistant

Sensors appear automatically under a **NerdMiner Solar** device thanks to MQTT auto-discovery. No `configuration.yaml` changes needed.

After first boot, go to:
**Settings → Devices & Services → MQTT** and you'll find the **NerdMiner Solar** device with all sensors grouped together.

---

## 📁 Files changed from NerdMinerV2

| File | Change |
|---|---|
| `src/mqtt_manager.h` | **New file** — all MQTT and scheduling logic |
| `src/NerdMinerV2.ino.cpp` | Added 3 includes and hook into setup()/loop() |
| `platformio.ini` | Added `knolleary/PubSubClient@^2.8` to ESP32-C3-super-mini env |

---

## 🔧 How to build

1. Clone this repo
2. Open in VS Code with PlatformIO extension installed
3. Edit `src/mqtt_manager.h` with your MQTT broker IP and coordinates
4. Select environment `ESP32-C3-super-mini` in the PlatformIO toolbar
5. Click Build then Upload

---

## 🌍 Adapting for other locations

Change these two lines in `mqtt_manager.h`:

```cpp
#define LOCATION_LAT     "YOUR_LATITUDE"
#define LOCATION_LNG     "YOUR_LONGITUDE"
```

And update the timezone string in `mqttTimeInit()` if outside the UK.

---

## 📜 Licence

Based on [NerdMinerV2](https://github.com/BitMaker-hub/NerdMiner_v2) by BitMaker. 
Additional MQTT and scheduling code released under MIT licence.

---

## 🙏 Credits

- [BitMaker](https://github.com/BitMaker-hub) — NerdMinerV2 project
- [sunrise-sunset.org](https://sunrise-sunset.org/api) — Free sunrise/sunset API
- [knolleary/PubSubClient](https://github.com/knolleary/pubsubclient) — MQTT library

<a href="https://www.buymeacoffee.com/MrSossidge" target="_blank"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me a Coffee" style="height: 60px !important;width: 217px !important;" ></a>
