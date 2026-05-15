#pragma once
// ═══════════════════════════════════════════════════════════════
// mqtt_manager.h — Solar NerdMiner MQTT & Mining Window Manager
//
// Handles:
//   - MQTT connection and publishing to Home Assistant
//   - Daily sunrise/sunset fetch from sunrise-sunset.org API
//     for North Ferriby, East Yorkshire (53.7183, -0.4746)
//   - UK timezone: GMT (winter) and BST (summer) auto-detection
//   - Mining window = civil twilight start/end ± 30 minutes
//   - HA auto-discovery so sensors appear automatically
//   - Deep sleep outside mining window to protect batteries
// ═══════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ── User configuration ────────────────────────────────────────
#define MQTT_BROKER      "YOUR_MQTT_BROKER_IP_OR_HOSTNAME"
#define MQTT_PORT        1883
#define MQTT_USER        ""
#define MQTT_PASS        ""
#define MQTT_CLIENT_ID   "nerdminer_solar"
#define MQTT_BASE_TOPIC  "nerdminer/solar"
#define MQTT_DISCO_PFX   "homeassistant"

// Location for sunrise/sunset API (Find your latitude and longitude at https://www.latlong.net/)
#define LOCATION_LAT     "YOUR_LATITUDE"
#define LOCATION_LNG     "YOUR_LONGITUDE"

// Buffer added/subtracted from civil twilight times
#define TWILIGHT_BUFFER_SECS  1800  // 30 minutes

// How often to publish stats to MQTT
#define MQTT_INTERVAL_MS  60000UL

// Refresh solar times once per day
#define SOLAR_REFRESH_MS  86400000UL
// ─────────────────────────────────────────────────────────────

static WiFiClient   mqttWifiClient;
static PubSubClient mqttClient(mqttWifiClient);
static unsigned long lastMqttPublish = 0;
static unsigned long lastSolarFetch  = 0;
static bool discoveryPublished       = false;

// Mining window stored as seconds since midnight (local UK time)
// Exposed globally so NerdMinerV2.ino.cpp can use them for sleep duration
int miningStartSecs   = 0;
int miningEndSecs     = 86400;
int twilightStartSecs = 0;
int twilightEndSecs   = 86400;

// ── Converts UTC time from sunrise-sunset.org API to UK local ─
// Handles both GMT (winter) and BST (summer) automatically
// Uses NerdMiner's stored timezone offset + DST detection
// API ISO8601 format: "2026-05-14T04:16:55+00:00"
// Location: North Ferriby, East Yorkshire (53.7183, -0.4746)
int utcIsoToLocalSecsSinceMidnight(const char* isoStr) {
  int year, month, day, hh, mm, ss;
  sscanf(isoStr, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hh, &mm, &ss);

  extern TSettings Settings;
  int offsetSecs = Settings.Timezone * 3600;

  time_t now = time(nullptr);
  struct tm localTm;
  localtime_r(&now, &localTm);
  if (localTm.tm_isdst > 0) {
    offsetSecs += 3600;
    Serial.println("[SOLAR] BST detected, adding 1 hour to offset");
  }

  int totalSecs = hh * 3600 + mm * 60 + ss + offsetSecs;
  totalSecs = totalSecs % 86400;
  if (totalSecs < 0) totalSecs += 86400;

  return totalSecs;
}

// ── Returns current time as seconds since midnight (local) ────
int localSecsSinceMidnight() {
  struct tm t;
  if (!getLocalTime(&t)) return 43200;
  return t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

// ── Fetches today's civil twilight times from sunrise-sunset.org
// Called once at boot and then refreshed daily at 00:05 local time
void fetchSolarTimes() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = "https://api.sunrise-sunset.org/json?lat="
               LOCATION_LAT "&lng=" LOCATION_LNG "&formatted=0";

  Serial.println("[SOLAR] Fetching civil twilight times...");
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, payload);

    if (!err) {
      const char* twilightBegin = doc["results"]["civil_twilight_begin"];
      const char* twilightEnd   = doc["results"]["civil_twilight_end"];

      int startSecs = utcIsoToLocalSecsSinceMidnight(twilightBegin);
      int endSecs   = utcIsoToLocalSecsSinceMidnight(twilightEnd);

      // Store raw twilight times for publishing to HA
      twilightStartSecs = startSecs;
      twilightEndSecs   = endSecs;

      // Apply ±30 minute buffer around civil twilight for mining window
      miningStartSecs = startSecs - TWILIGHT_BUFFER_SECS;
      miningEndSecs   = endSecs   + TWILIGHT_BUFFER_SECS;

      // Clamp to day boundaries
      if (miningStartSecs < 0)     miningStartSecs = 0;
      if (miningEndSecs   > 86400) miningEndSecs   = 86400;

      int sh = miningStartSecs / 3600, sm = (miningStartSecs % 3600) / 60;
      int eh = miningEndSecs   / 3600, em = (miningEndSecs   % 3600) / 60;
      int th = twilightStartSecs / 3600, tm2 = (twilightStartSecs % 3600) / 60;
      int teh = twilightEndSecs / 3600, tem = (twilightEndSecs % 3600) / 60;
      Serial.printf("[SOLAR] Twilight: %02d:%02d → %02d:%02d | Mining window: %02d:%02d → %02d:%02d (UK local)\n",
                    th, tm2, teh, tem, sh, sm, eh, em);

      lastSolarFetch = millis();
    } else {
      Serial.println("[SOLAR] JSON parse error — keeping previous window");
    }
  } else {
    Serial.printf("[SOLAR] HTTP error %d — keeping previous window\n", httpCode);
  }

  http.end();
}

// ── Returns true if current UK local time is within mining window
// Solar times are refreshed daily just after midnight (00:05 UK local)
bool isMiningWindow() {
  struct tm t;
  getLocalTime(&t);
  int nowSecs = localSecsSinceMidnight();

  // Refresh at 00:05 each day — fresh times ready before morning window opens
  static int lastRefreshDay = -1;
  if (t.tm_mday != lastRefreshDay && nowSecs >= 300) {
    Serial.println("[SOLAR] Daily refresh triggered at 00:05...");
    fetchSolarTimes();
    lastRefreshDay = t.tm_mday;
  }

  // Also fetch immediately on first boot
  if (lastSolarFetch == 0) {
    fetchSolarTimes();
  }

  bool mining = (nowSecs >= miningStartSecs && nowSecs <= miningEndSecs);

  int sh = miningStartSecs / 3600, sm = (miningStartSecs % 3600) / 60;
  int eh = miningEndSecs   / 3600, em = (miningEndSecs   % 3600) / 60;
  int nh = nowSecs / 3600,         nm = (nowSecs % 3600) / 60;
  Serial.printf("[SOLAR] Now: %02d:%02d | Window: %02d:%02d→%02d:%02d | Mining: %s\n",
                nh, nm, sh, sm, eh, em, mining ? "YES" : "NO");

  return mining;
}

// ── Initialises UK timezone and syncs NTP ─────────────────────
void mqttTimeInit() {
  configTzTime("GMT0BST,M3.5.0/1,M10.5.0", "pool.ntp.org", "time.cloudflare.com");
  Serial.print("[TIME] Syncing NTP");
  struct tm t;
  int attempts = 0;
  while (!getLocalTime(&t) && attempts++ < 20) {
    Serial.print(".");
    delay(500);
  }
  Serial.println(t.tm_year > 100 ? " OK" : " FAILED");
}

// ── Publishes HA auto-discovery config for all sensors ────────
void publishDiscovery() {
  struct SensorDef {
    const char* id;
    const char* name;
    const char* stateTopic;
    const char* unit;
    const char* icon;
  };

  SensorDef sensors[] = {
    {"hashrate",       "NerdMiner Hash Rate",     MQTT_BASE_TOPIC "/hashrate",       "KH/s", "mdi:pickaxe"            },
    {"shares",         "NerdMiner Shares",         MQTT_BASE_TOPIC "/shares",         "",     "mdi:bitcoin"            },
    {"total_kh",       "NerdMiner Total KH",       MQTT_BASE_TOPIC "/total_kh",       "KH",   "mdi:sigma"              },
    {"uptime",         "NerdMiner Uptime",         MQTT_BASE_TOPIC "/uptime",         "",     "mdi:timer-outline"      },
    {"valids",         "NerdMiner Valid Blocks",   MQTT_BASE_TOPIC "/valids",         "",     "mdi:check-circle"       },
    {"mining_active",  "NerdMiner Mining Active",  MQTT_BASE_TOPIC "/mining_active",  "",     "mdi:power"              },
    {"window_start",   "NerdMiner Window Start",   MQTT_BASE_TOPIC "/window_start",   "",     "mdi:weather-sunset-up"  },
    {"window_end",     "NerdMiner Window End",     MQTT_BASE_TOPIC "/window_end",     "",     "mdi:weather-sunset"     },
    {"twilight_start", "NerdMiner Twilight Start", MQTT_BASE_TOPIC "/twilight_start", "",     "mdi:white-balance-sunny"},
    {"twilight_end",   "NerdMiner Twilight End",   MQTT_BASE_TOPIC "/twilight_end",   "",     "mdi:moon-waning-crescent"},
  };

  String deviceBlock =
    "\"device\":{"
    "\"identifiers\":[\"nerdminer_solar\"],"
    "\"name\":\"NerdMiner Solar\","
    "\"model\":\"ESP32-C3 SuperMini\","
    "\"manufacturer\":\"DIY\""
    "}";

  for (auto& s : sensors) {
    String topic = String(MQTT_DISCO_PFX) + "/sensor/nerdminer_solar/" + s.id + "/config";
    String payload = "{";
    payload += "\"unique_id\":\"nerdminer_solar_" + String(s.id) + "\",";
    payload += "\"name\":\"" + String(s.name) + "\",";
    payload += "\"state_topic\":\"" + String(s.stateTopic) + "\",";
    if (strlen(s.unit) > 0)
      payload += "\"unit_of_measurement\":\"" + String(s.unit) + "\",";
    payload += "\"icon\":\"" + String(s.icon) + "\",";
    payload += deviceBlock;
    payload += "}";

    mqttClient.setBufferSize(512);
    mqttClient.publish(topic.c_str(), payload.c_str(), true);
    delay(50);
  }

  Serial.println("[MQTT] HA auto-discovery published.");
  discoveryPublished = true;
}

// ── Connects to MQTT broker ───────────────────────────────────
void mqttConnect() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) return;

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);

  Serial.println("[MQTT] Connecting to broker...");
  int retries = 0;
  while (!mqttClient.connected() && retries < 5) {
    bool ok = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS);
    if (ok) {
      Serial.println("[MQTT] Connected.");
      mqttClient.publish(MQTT_BASE_TOPIC "/status", "online", true);
      discoveryPublished = false;
    } else {
      Serial.printf("[MQTT] Failed (rc=%d), retry %d/5\n", mqttClient.state(), ++retries);
      delay(2000);
    }
  }
}

// ── Publishes mining stats to MQTT every 60 seconds ──────────
void mqttPublishStats(String hashRate, String shares,
                      String totalKH, String uptime,
                      String valids, bool mining) {
  if (!mqttClient.connected()) mqttConnect();
  if (!mqttClient.connected()) return;

  if (!discoveryPublished) publishDiscovery();

  unsigned long now = millis();
  if (now - lastMqttPublish < MQTT_INTERVAL_MS) return;
  lastMqttPublish = now;

  mqttClient.publish(MQTT_BASE_TOPIC "/hashrate",      hashRate.c_str(),          true);
  mqttClient.publish(MQTT_BASE_TOPIC "/shares",        shares.c_str(),            true);
  mqttClient.publish(MQTT_BASE_TOPIC "/total_kh",      totalKH.c_str(),           true);
  mqttClient.publish(MQTT_BASE_TOPIC "/uptime",        uptime.c_str(),            true);
  mqttClient.publish(MQTT_BASE_TOPIC "/valids",        valids.c_str(),            true);
  mqttClient.publish(MQTT_BASE_TOPIC "/mining_active", mining ? "true" : "false", true);

  // Publish buffered mining window times
  char startStr[6], endStr[6];
  snprintf(startStr, sizeof(startStr), "%02d:%02d",
           miningStartSecs / 3600, (miningStartSecs % 3600) / 60);
  snprintf(endStr, sizeof(endStr), "%02d:%02d",
           miningEndSecs / 3600, (miningEndSecs % 3600) / 60);
  mqttClient.publish(MQTT_BASE_TOPIC "/window_start", startStr, true);
  mqttClient.publish(MQTT_BASE_TOPIC "/window_end",   endStr,   true);

  // Publish raw civil twilight times
  char twilightStartStr[6], twilightEndStr[6];
  snprintf(twilightStartStr, sizeof(twilightStartStr), "%02d:%02d",
           twilightStartSecs / 3600, (twilightStartSecs % 3600) / 60);
  snprintf(twilightEndStr, sizeof(twilightEndStr), "%02d:%02d",
           twilightEndSecs / 3600, (twilightEndSecs % 3600) / 60);
  mqttClient.publish(MQTT_BASE_TOPIC "/twilight_start", twilightStartStr, true);
  mqttClient.publish(MQTT_BASE_TOPIC "/twilight_end",   twilightEndStr,   true);

  Serial.printf("[MQTT] Published — HR:%s Shares:%s Mining:%s Window:%s→%s Twilight:%s→%s\n",
                hashRate.c_str(), shares.c_str(),
                mining ? "YES" : "NO",
                startStr, endStr,
                twilightStartStr, twilightEndStr);
}

// ── Must be called in main loop() to keep MQTT connection alive
void mqttLoop() {
  mqttClient.loop();
}