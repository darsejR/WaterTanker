/*
  ESP32-C3 WaterNode — Admin UI + First-boot Portal + WiFi/MQTT + Sonar + Home Assistant Discovery (robust)

  - First boot AP portal: "WaterNode-XXXX" at http://192.168.4.1/
  - Admin UI at http://<device-ip>/
  - Test Wi-Fi & MQTT (3 attempts, modal + spinner, no page nav)
  - HA MQTT Discovery: now with configurable discovery prefix (default "homeassistant"),
    explicit per-topic success logging, and a "Re-publish Discovery" button (/rediscover).
  - Retained config topics; availability via status topic; periodic JSON states.

  Board: ESP32C3 Dev Module
  Tools -> USB CDC On Boot: Enabled
  Libraries: PubSubClient (Nick O’Leary)
  Wiring: HC-SR04  TRIG=GPIO4, ECHO (via divider 10k/15k!)=GPIO5
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <math.h>

// ---------- Pins / Sonar ----------
const int PIN_TRIG = 4;
const int PIN_ECHO = 5;  // IMPORTANT: ECHO via divider!

// ---------- Defaults ----------
const uint32_t PUBLISH_MINUTES_DEFAULT = 1;      // bump for production
const int      TANK_DEPTH_MM_DEFAULT    = 1500;
const int      SENSOR_OFFSET_MM_DEFAULT = 25;

// Tank shapes
enum TankShape : uint8_t { SHAPE_CUSTOM = 0, SHAPE_CYL = 1, SHAPE_RECT = 2 };

// ---------- Force-portal button ----------
#define FORCE_PORTAL_BUTTON 9
const uint32_t FORCE_PRESS_MS = 2000;

// ========= Storage =========
Preferences prefs;

struct Config {
  String wifiSsid;
  String wifiPass;

  String mqttHost;
  uint16_t mqttPort;
  String mqttUser;
  String mqttPass;

  String clientId;       // also used for HA discovery identifiers
  String topicData;      // JSON state topic
  String topicStatus;    // availability (online/offline)

  String discoveryPrefix; // NEW: HA discovery prefix (default "homeassistant")

  uint32_t publishMinutes;
  int tankDepth;
  int sensorOffset;

  // Tank geometry
  uint8_t shape;          // 0=custom area; 1=cylinder; 2=rectangular
  int cylDiameterMm;      // for cylinder
  int rectLenMm;          // for rectangular
  int rectWidMm;          // for rectangular
  int customAreaMm2;      // cross-section area (mm^2) if SHAPE_CUSTOM

  bool configured;
};

Config cfg;

// ====== Reading BEFORE any functions (Arduino auto-prototypes) ======
struct Reading {
  long   distance_mm = -1;
  int    level_mm    = -1;
  double percent     = -1.0;
  double liters      = -1.0;
};

// ====== Sticky form cache (so Test doesn’t wipe your inputs) ======
struct TempForm {
  bool   valid = false;
  String wss, wps, mh, mport, mu, mx, cid, td, ts, dpfx;
  String iv, tdp, ofs, shape, cyl, rlen, rwid, area;
} lastForm;

// ========= Forwards =========
String htmlStatus();
String htmlConfig();
void publishHADiscovery();

// ========= Net / MQTT / Web =========
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

DNSServer dns;          // portal
WebServer server(80);
IPAddress  apIP(192,168,4,1);
String     apSsid;

static volatile bool wifiConnecting   = false;
static uint32_t     nextWifiAttemptMs = 0;
static uint8_t      assocFailCount    = 0;

// ========= SONAR =========
static void sonarInit() {
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);
  delay(50);
  Serial.printf("[SONAR] init done. TRIG=%d ECHO=%d\n", PIN_TRIG, PIN_ECHO);
}

static long sonarReadOnce(uint32_t echo_timeout_us = 30000) {
  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long us = pulseIn(PIN_ECHO, HIGH, echo_timeout_us);
  if (us == 0) return -1;
  return (long)((float)us / 5.8f); // µs → mm for HC-SR04
}

static long sonarReadStable() {
  const int N = 5;
  long v[N];
  for (int i = 0; i < N; ++i) { v[i] = sonarReadOnce(); delay(60); }
  for (int i = 1; i < N; ++i) { long k=v[i]; int j=i-1; while(j>=0 && v[j]>k){v[j+1]=v[j]; j--;} v[j+1]=k; }
  long m = v[N/2]; if (m < 0) return -1;
  long sum=0; int cnt=0;
  for (int i=1;i<N-1;++i){ if(v[i]>0 && labs(v[i]-m)<300){ sum+=v[i]; cnt++; } }
  return cnt ? (sum/cnt) : m;
}

// ========= Tank math =========
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double crossSectionArea_mm2(const Config& c) {
  if (c.shape == SHAPE_CYL && c.cylDiameterMm > 0) {
    double d = (double)c.cylDiameterMm;
    return (M_PI * d * d) / 4.0;
  } else if (c.shape == SHAPE_RECT && c.rectLenMm > 0 && c.rectWidMm > 0) {
    return (double)c.rectLenMm * (double)c.rectWidMm;
  } else if (c.customAreaMm2 > 0) {
    return (double)c.customAreaMm2;
  }
  return 0.0;
}

static Reading takeReading() {
  Reading r;
  long distance = sonarReadStable();
  if (distance > 0) { delay(100); long d2 = sonarReadStable(); if (d2 > 0) distance = (distance + d2) / 2; }
  r.distance_mm = distance;

  if (distance > 0) {
    int level = cfg.tankDepth - distance - cfg.sensorOffset;
    if (level < 0) level = 0;
    if (level > cfg.tankDepth) level = cfg.tankDepth;
    r.level_mm = level;
    r.percent  = 100.0 * (double)level / (double)cfg.tankDepth;

    double A = crossSectionArea_mm2(cfg);
    if (A > 0) {
      double vol_mm3 = A * (double)level;
      r.liters = vol_mm3 / 1e6;
    }
  }
  return r;
}

// ========= Preferences =========
void loadConfig() {
  prefs.begin("waternode", true);
  cfg.configured     = prefs.getBool("configured", false);
  cfg.wifiSsid       = prefs.getString("wifiSsid", "");
  cfg.wifiPass       = prefs.getString("wifiPass", "");
  cfg.mqttHost       = prefs.getString("mqttHost", "");
  cfg.mqttPort       = prefs.getUShort("mqttPort", 1883);
  cfg.mqttUser       = prefs.getString("mqttUser", "");
  cfg.mqttPass       = prefs.getString("mqttPass", "");
  cfg.clientId       = prefs.getString("clientId",  "tank1-esp32c3");
  cfg.topicData      = prefs.getString("topicData", "sensors/water/tank1");
  cfg.topicStatus    = prefs.getString("topicStat", "sensors/water/tank1/status");
  cfg.discoveryPrefix= prefs.getString("discPref",  "homeassistant"); // NEW
  cfg.publishMinutes = prefs.getUInt("pubMin",      PUBLISH_MINUTES_DEFAULT);
  cfg.tankDepth      = prefs.getInt("tankDepth",    TANK_DEPTH_MM_DEFAULT);
  cfg.sensorOffset   = prefs.getInt("sensOffset",   SENSOR_OFFSET_MM_DEFAULT);

  cfg.shape          = prefs.getUChar("shape",       (uint8_t)SHAPE_CUSTOM);
  cfg.cylDiameterMm  = prefs.getInt("cylDia",        0);
  cfg.rectLenMm      = prefs.getInt("rectLen",       0);
  cfg.rectWidMm      = prefs.getInt("rectWid",       0);
  cfg.customAreaMm2  = prefs.getInt("area",          0);
  prefs.end();
}

void saveConfig() {
  prefs.begin("waternode", false);
  prefs.putBool(   "configured", true);
  prefs.putString( "wifiSsid",   cfg.wifiSsid);
  prefs.putString( "wifiPass",   cfg.wifiPass);
  prefs.putString( "mqttHost",   cfg.mqttHost);
  prefs.putUShort( "mqttPort",   cfg.mqttPort);
  prefs.putString( "mqttUser",   cfg.mqttUser);
  prefs.putString( "mqttPass",   cfg.mqttPass);
  prefs.putString( "clientId",   cfg.clientId);
  prefs.putString( "topicData",  cfg.topicData);
  prefs.putString( "topicStat",  cfg.topicStatus);
  prefs.putString( "discPref",   cfg.discoveryPrefix); // NEW
  prefs.putUInt(   "pubMin",     cfg.publishMinutes);
  prefs.putInt(    "tankDepth",  cfg.tankDepth);
  prefs.putInt(    "sensOffset", cfg.sensorOffset);

  prefs.putUChar(  "shape",      cfg.shape);
  prefs.putInt(    "cylDia",     cfg.cylDiameterMm);
  prefs.putInt(    "rectLen",    cfg.rectLenMm);
  prefs.putInt(    "rectWid",    cfg.rectWidMm);
  prefs.putInt(    "area",       cfg.customAreaMm2);
  prefs.end();
}

// ========= Helpers =========
static void logAdminURL() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[HTTP] Admin UI available at http://%s/\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[HTTP] Admin UI available at http://<device-ip>/ (waiting for DHCP)");
  }
}

// ========= Wi-Fi STA =========
static void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      Serial.println("[WIFI] STA start"); break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[WIFI] STA connected (auth OK)"); break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiConnecting = false; assocFailCount = 0;
      Serial.printf("[WIFI] Got IP: %s  RSSI=%d\n",
        WiFi.localIP().toString().c_str(), WiFi.RSSI());
      logAdminURL();
      if (mqtt.connected()) { delay(50); publishHADiscovery(); }
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      int r = (int)info.wifi_sta_disconnected.reason;
      Serial.printf("[WIFI] Disconnected. reason=%d\n", r);
      wifiConnecting = false;
      if (r == 2 || r == 4) assocFailCount++;
      nextWifiAttemptMs = millis() + 3000;
      break;
    }
    default: break;
  }
}

static void wifiInitSTA() {
  WiFi.onEvent(onWiFiEvent);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
}

static void wifiKickOnce() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (wifiConnecting) return;
  if (millis() < nextWifiAttemptMs) return;
  Serial.printf("[WIFI] Connecting to %s ...\n", cfg.wifiSsid.c_str());
  WiFi.disconnect(true, true);
  delay(150);
  wifiConnecting = true;
  WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
  nextWifiAttemptMs = millis() + 15000;
}

// ========= MQTT =========
static uint32_t nextMqttAttemptMs = 0;

static void mqttInit() {
  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
  mqtt.setBufferSize(1024);   // <<< add this line
}

static bool mqttPublishRetained(const String& topic, const String& payload) {
  bool ok = mqtt.publish(topic.c_str(), payload.c_str(), true);
  Serial.printf("[HA] Publish %s  retain=1  -> %s\n", topic.c_str(), ok ? "OK" : "FAIL");
  if (!ok) Serial.printf("[HA] Payload: %s\n", payload.c_str());
  return ok;
}

// ---- Home Assistant Discovery ----
static String haDeviceJson() {
  String cfgUrl;
  if (WiFi.status() == WL_CONNECTED) cfgUrl = "http://" + WiFi.localIP().toString() + "/";
  String j = "{";
  j += "\"identifiers\":[\"waternode:" + cfg.clientId + "\"],";
  j += "\"name\":\"WaterNode " + cfg.clientId + "\",";
  j += "\"manufacturer\":\"DIY\",";
  j += "\"model\":\"ESP32-C3 WaterNode\",";
  j += "\"sw_version\":\"1.0\"";
  if (cfgUrl.length()) j += ",\"configuration_url\":\"" + cfgUrl + "\"";
  j += "}";
  return j;
}

void publishHADiscovery() {
  if (!mqtt.connected()) { Serial.println("[HA] Discovery skipped: MQTT not connected"); return; }

  const String base = cfg.discoveryPrefix + "/sensor/"; // NEW: configurable prefix
  const String dev  = haDeviceJson();
  const String avty = cfg.topicStatus;

  auto cfgSensor = [&](const char* key, const char* friendly, const char* unit,
                       const char* device_class, const char* state_class,
                       const char* icon, const char* value_template,
                       const char* entity_category) {
    String topic = base + cfg.clientId + "_" + key + "/config";
    String p = "{";
    p += "\"name\":\"" + String(friendly) + "\",";
    p += "\"unique_id\":\"" + cfg.clientId + "_" + key + "\",";
    p += "\"state_topic\":\"" + cfg.topicData + "\",";
    p += "\"availability_topic\":\"" + avty + "\",";
    p += "\"payload_available\":\"online\",";
    p += "\"payload_not_available\":\"offline\",";
    if (unit && *unit)                  p += "\"unit_of_measurement\":\"" + String(unit) + "\",";
    if (device_class && *device_class)  p += "\"device_class\":\"" + String(device_class) + "\",";
    if (state_class && *state_class)    p += "\"state_class\":\"" + String(state_class) + "\",";
    if (entity_category && *entity_category) p += "\"entity_category\":\"" + String(entity_category) + "\",";
    if (icon && *icon)                  p += "\"icon\":\"" + String(icon) + "\",";
    p += "\"value_template\":\"" + String(value_template) + "\",";
    p += "\"device\":" + dev;
    p += "}";

    mqttPublishRetained(topic, p);
  };

  // Primary sensors
  cfgSensor("level_mm", "Water Level (mm)", "mm",
            nullptr, "measurement", "mdi:waves",
            "{{ value_json.level_mm | default(None) }}", nullptr);

  cfgSensor("level_pct", "Water Level (%)", "%",
            nullptr, "measurement", "mdi:percent",
            "{{ value_json.percent | default(None) }}", nullptr);

  // Diagnostics / optional
  cfgSensor("liters", "Water Volume (L)", "L",
            "volume", "measurement", "mdi:cup-water",
            "{{ value_json.liters | default(None) }}", nullptr);

  cfgSensor("distance_mm", "Sonar Distance (mm)", "mm",
            nullptr, "measurement", "mdi:arrow-expand-vertical",
            "{{ value_json.distance_mm | default(None) }}", "diagnostic");

  cfgSensor("rssi", "Wi-Fi RSSI", "dBm",
            "signal_strength", "measurement", "mdi:wifi",
            "{{ value_json.rssi | default(None) }}", "diagnostic");

  Serial.println("[HA] Discovery config published (requested).");
}

static void mqttKickOnce() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) return;
  if (millis() < nextMqttAttemptMs) return;

  Serial.printf("[MQTT] Connecting to %s:%u ...\n", cfg.mqttHost.c_str(), cfg.mqttPort);

  // LWT must be *plain text* "offline" to match discovery
  bool ok;
  if (cfg.mqttUser.length()) {
    ok = mqtt.connect(
      cfg.clientId.c_str(),
      cfg.mqttUser.c_str(),
      cfg.mqttPass.c_str(),
      cfg.topicStatus.c_str(), 1, true, "offline"   // <— LWT payload: "offline"
    );
  } else {
    ok = mqtt.connect(
      cfg.clientId.c_str(),
      nullptr, nullptr,
      cfg.topicStatus.c_str(), 1, true, "offline"   // <— LWT payload: "offline"
    );
  }

  if (ok) {
    // Announce availability with *plain text* "online", retained
    mqtt.publish(cfg.topicStatus.c_str(), "online", true);   // <— not JSON
    Serial.println("[MQTT] Connected.");
    publishHADiscovery();  // fire discovery after a good connect
    nextMqttAttemptMs = 0;
  } else {
    Serial.printf("[MQTT] Connect failed. rc=%d\n", mqtt.state());
    nextMqttAttemptMs = millis() + 5000;
  }
}


// ========= Measure + Publish =========
static void measureAndPublish() {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[NET] Skip publish: Wi-Fi down"); return; }
  if (!mqtt.connected())              { Serial.println("[NET] Skip publish: MQTT down"); return; }

  Reading r = takeReading();

  char payload[360];
  int rssi = WiFi.RSSI();
  if (r.distance_mm > 0) {
    if (r.liters >= 0) {
      snprintf(payload, sizeof(payload),
        "{\"distance_mm\":%ld,\"level_mm\":%d,\"percent\":%.1f,\"liters\":%.2f,\"rssi\":%d}",
        r.distance_mm, r.level_mm, r.percent, r.liters, rssi);
    } else {
      snprintf(payload, sizeof(payload),
        "{\"distance_mm\":%ld,\"level_mm\":%d,\"percent\":%.1f,\"rssi\":%d}",
        r.distance_mm, r.level_mm, r.percent, rssi);
    }
  } else {
    snprintf(payload, sizeof(payload),
      "{\"error\":\"no_read\",\"rssi\":%d}", rssi);
  }

  bool ok = mqtt.publish(cfg.topicData.c_str(), payload, false);
  Serial.printf("[PUBLISH] %s  -> %s\n", payload, ok ? "OK" : "FAIL");
}

// ========= HTML (UI + spinner modal) =========
const char HTML_HDR[] PROGMEM =
  "<!DOCTYPE html><html><head><meta charset='utf-8'>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>WaterNode</title>"
  "<style>"
  ":root{--bg:#0b1020;--card:#131a2a;--text:#e8f0ff;--muted:#9bb0d0;--ac:#4ea1ff;--ok:#21c07a;--bad:#ff5b6e}"
  "body{font-family:system-ui,Segoe UI,Arial;margin:0;background:linear-gradient(180deg,#0b1020,#0f1530);color:var(--text)}"
  ".wrap{max-width:980px;margin:32px auto;padding:0 16px}"
  ".header{display:flex;justify-content:space-between;align-items:center;margin:8px 0 20px}"
  ".title{font-weight:700;font-size:28px;letter-spacing:.3px}"
  ".card{background:var(--card);border-radius:16px;padding:18px 18px 12px;box-shadow:0 8px 24px rgba(0,0,0,.35);margin:14px 0}"
  "label{display:block;margin:10px 0 6px;color:var(--muted);font-size:13px}"
  "input,select{width:100%;padding:10px 12px;border:1px solid #233048;background:#0c1424;color:var(--text);border-radius:10px;outline:none}"
  "input:focus,select:focus{border-color:var(--ac)}"
  "fieldset{border:none;padding:0;margin:0}"
  ".row{display:grid;grid-template-columns:1fr 1fr;gap:12px}"
  ".btns{display:flex;gap:12px;flex-wrap:wrap;margin-top:14px}"
  "button,.btn{display:inline-block;background:var(--ac);border:none;color:#001628;padding:11px 14px;border-radius:12px;font-weight:700;cursor:pointer;text-decoration:none;text-align:center}"
  "button.secondary,.btn.secondary{background:#2a3550;color:var(--text)}"
  ".small{font-size:12px;color:var(--muted)}"
  ".kv{display:flex;gap:10px;flex-wrap:wrap}"
  ".pill{background:#1b2740;border:1px solid #2b3b60;padding:6px 10px;border-radius:999px;font-size:12px}"
  ".ok{color:var(--ok);font-weight:700}.bad{color:var(--bad);font-weight:700}"
  "@media(max-width:700px){.row{grid-template-columns:1fr}}"
  "a{color:var(--ac)}"
  /* Modal */
  "#modal{position:fixed;inset:0;background:rgba(0,0,0,.55);display:none;align-items:center;justify-content:center;padding:16px;z-index:9999}"
  ".modal-card{background:#0e1426;border:1px solid #2b3b60;border-radius:16px;max-width:560px;width:100%;box-shadow:0 10px 30px rgba(0,0,0,.5)}"
  ".modal-card header{padding:14px 16px;border-bottom:1px solid #2b3b60;font-weight:700}"
  ".modal-card .content{padding:14px 16px}"
  ".modal-card .actions{display:flex;justify-content:end;gap:10px;padding:0 16px 14px}"
  /* Spinner */
  ".spinner{width:28px;height:28px;border:3px solid #2b3b60;border-top-color:var(--ac);border-radius:50%;animation:spin .9s linear infinite;display:inline-block;vertical-align:middle}"
  "@keyframes spin{to{transform:rotate(360deg)}}"
  ".testing-note{display:flex;align-items:center;gap:10px;color:var(--muted)}"
  "button[disabled]{opacity:.6;cursor:not-allowed}"
  "</style>"
  "<script>"
  "async function doTest(ev){"
    "ev.preventDefault();"
    "const btn = ev.target;"
    "const form = document.getElementById('cfg');"
    "document.getElementById('modal-body').innerHTML = \"<div class='testing-note'><span class='spinner'></span><span>Testing Wi-Fi & MQTT…</span></div>\";"
    "document.getElementById('modal').style.display='flex';"
    "const wasDisabled = btn.disabled; btn.disabled = true;"
    "try{"
      "const fd = new FormData(form);"
      "const res = await fetch('/test.json',{method:'POST',body:fd});"
      "const j = await res.json();"
      "const w = j.wifi, m = j.mqtt;"
      "let html='';"
      "html += `<p>Wi-Fi: ${w.ok?'<span class=ok>OK</span>':'<span class=bad>FAIL</span>'}${w.ip?(' — IP '+w.ip):''}</p>`;"
      "if(w.attempts&&w.attempts.length){html += '<ul class=small>'+w.attempts.map(x=>`<li>${x}</li>`).join('')+'</ul>';}"
      "html += `<p>MQTT: ${m.ok?'<span class=ok>OK</span>':'<span class=bad>FAIL</span>'}</p>`;"
      "if(m.attempts&&m.attempts.length){html += '<ul class=small>'+m.attempts.map(x=>`<li>${x}</li>`).join('')+'</ul>';}"
      "document.getElementById('modal-body').innerHTML = html;"
    "}catch(e){"
      "document.getElementById('modal-body').innerHTML = \"<p class='bad'>Test failed to run.</p><p class='small'>\"+String(e)+\"</p>\";"
    "}finally{"
      "btn.disabled = wasDisabled;"
    "}"
  "}"
  "function closeModal(){document.getElementById('modal').style.display='none';}"
  "</script>"
  "</head><body><div class='wrap'>"
  "<div class='header'><div class='title'>WaterNode</div><div class='kv'>"
  "<span class='pill'>Portal</span>"
  "<a class='pill' href='/status.json'>status.json</a>"
  "<a class='pill' href='/reboot'>reboot</a>"
  "<a class='pill' href='/factory'>factory</a>"
  "<a class='pill' href='/rediscover'>rediscover</a>"
  "</div></div>"
  "<div id='modal'><div class='modal-card'><header>Connection Test</header><div class='content' id='modal-body'>...</div><div class='actions'><button class='secondary' onclick='closeModal()'>Close</button></div></div></div>";

const char HTML_FTR[] PROGMEM = "</div></body></html>";

static String fieldOr(const String& fromForm, const String& fromCfg) {
  return lastForm.valid ? fromForm : fromCfg;
}

String htmlStatus() {
  Reading r = takeReading();
  String h = FPSTR(HTML_HDR);
  h += "<div class='card'><h3>Status</h3>";
  if (WiFi.status() == WL_CONNECTED) {
    h += "<p class='small'>IP: " + WiFi.localIP().toString() + " &nbsp; RSSI: " + String(WiFi.RSSI()) + " dBm</p>";
  } else {
    h += "<p class='small'>Wi-Fi disconnected</p>";
  }
  if (r.distance_mm > 0) {
    h += "<p><b>Distance:</b> " + String(r.distance_mm) + " mm<br>";
    h += "<b>Level:</b> " + String(r.level_mm) + " mm (" + String(r.percent,1) + "%)";
    if (r.liters >= 0) h += "<br><b>Volume:</b> " + String(r.liters,2) + " L";
    h += "</p>";
  } else {
    h += "<p><b>Last read:</b> no_read</p>";
  }
  h += "<div class='btns'><a class='btn secondary' href='/config'>Configure</a></div></div>";
  h += FPSTR(HTML_FTR);
  return h;
}

String htmlConfig() {
  String h = FPSTR(HTML_HDR);
  h += "<div class='card'><h3>Configuration</h3>";
  h += "<form id='cfg' method='POST' action='/save'>";

  h += "<fieldset><legend class='small'>Wi-Fi</legend>";
  h += "<label>SSID</label><input name='wss' value='" + fieldOr(lastForm.wss, cfg.wifiSsid) + "' required>";
  h += "<label>Password</label><input name='wps' type='password' value='" + fieldOr(lastForm.wps, cfg.wifiPass) + "'>";
  h += "</fieldset>";

  h += "<fieldset style='margin-top:10px'><legend class='small'>MQTT</legend>";
  h += "<div class='row'><div><label>Host</label><input name='mh' value='" + fieldOr(lastForm.mh, cfg.mqttHost) + "' required></div>";
  h += "<div><label>Port</label><input name='mp' type='number' min='1' max='65535' value='" + fieldOr(lastForm.mport, String(cfg.mqttPort)) + "'></div></div>";
  h += "<div class='row'><div><label>User</label><input name='mu' value='" + fieldOr(lastForm.mu, cfg.mqttUser) + "'></div>";
  h += "<div><label>Password</label><input name='mx' type='password' value='" + fieldOr(lastForm.mx, cfg.mqttPass) + "'></div></div>";
  h += "<div class='row'><div><label>Client ID</label><input name='cid' value='" + fieldOr(lastForm.cid, cfg.clientId) + "'></div>";
  h += "<div><label>Publish Interval (min)</label><input name='iv' type='number' min='1' max='1440' value='" + fieldOr(lastForm.iv, String(cfg.publishMinutes)) + "'></div></div>";
  h += "<div class='row'><div><label>Data Topic (JSON state)</label><input name='td' value='" + fieldOr(lastForm.td, cfg.topicData) + "'></div>";
  h += "<div><label>Status Topic (availability)</label><input name='ts' value='" + fieldOr(lastForm.ts, cfg.topicStatus) + "'></div></div>";
  h += "</fieldset>";

  h += "<fieldset style='margin-top:10px'><legend class='small'>Home Assistant</legend>";
  h += "<label>Discovery Prefix</label><input name='dpfx' value='" + fieldOr(lastForm.dpfx, cfg.discoveryPrefix) + "'>";
  h += "<p class='small'>This must match HA &rarr; MQTT integration &rarr; Discovery Prefix (default: <b>homeassistant</b>).</p>";
  h += "</fieldset>";

  h += "<fieldset style='margin-top:10px'><legend class='small'>Tank</legend>";
  h += "<div class='row'><div><label>Depth (mm)</label><input name='tdp' type='number' value='" + fieldOr(lastForm.tdp, String(cfg.tankDepth)) + "'></div>";
  h += "<div><label>Sensor Offset (mm)</label><input name='ofs' type='number' value='" + fieldOr(lastForm.ofs, String(cfg.sensorOffset)) + "'></div></div>";

  h += "<label>Shape</label><select name='shape'>";
  uint8_t shapeSel = lastForm.valid && lastForm.shape.length() ? (uint8_t)lastForm.shape.toInt() : cfg.shape;
  h += String("<option value='0'") + (shapeSel==SHAPE_CUSTOM?" selected":"") + ">Custom area (mm²)</option>";
  h += String("<option value='1'") + (shapeSel==SHAPE_CYL   ?" selected":"") + ">Cylinder</option>";
  h += String("<option value='2'") + (shapeSel==SHAPE_RECT  ?" selected":"") + ">Rectangular</option>";
  h += "</select>";

  h += "<div class='row'><div><label>Cylinder Diameter (mm)</label><input name='cyl' type='number' value='" + fieldOr(lastForm.cyl, String(cfg.cylDiameterMm)) + "'></div>";
  h += "<div><label>Rectangular Length (mm)</label><input name='rlen' type='number' value='" + fieldOr(lastForm.rlen, String(cfg.rectLenMm)) + "'></div></div>";
  h += "<div class='row'><div><label>Rectangular Width (mm)</label><input name='rwid' type='number' value='" + fieldOr(lastForm.rwid, String(cfg.rectWidMm)) + "'></div>";
  h += "<div><label>Custom Area (mm²)</label><input name='area' type='number' value='" + fieldOr(lastForm.area, String(cfg.customAreaMm2)) + "'></div></div>";

  h += "<p class='small'>Volume (L) = cross-section area × level / 1,000,000.</p>";

  // Buttons
  h += "<div class='btns'>";
  h += "<button type='button' onclick='doTest(event)' class='secondary'>Test Wi-Fi & MQTT</button>";
  h += "<a class='btn secondary' href='/rediscover'>Re-publish HA Discovery</a>";
  h += "<button type='submit' class='btn'>Save & Reboot</button>";
  h += "</div>";

  h += "</form></div>";
  h += FPSTR(HTML_FTR);
  return h;
}

// ========= JSON endpoints & helpers =========
static uint16_t clampPort(long p) { if (p < 1 || p > 65535) return 1883; return (uint16_t)p; }
static void jsonEscape(String& s) { s.replace("\\", "\\\\"); s.replace("\"", "\\\""); }

static void doWiFiTest3(const String& ssid, const String& pass, bool& ok, String& ip, String attempts[3], int& n) {
  ok = false; ip = ""; n = 0;
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  for (int a=0; a<3; ++a) {
    WiFi.disconnect(true, true);
    delay(100);
    unsigned long t0 = millis();
    WiFi.begin(ssid.c_str(), pass.c_str());
    wl_status_t st;
    bool connected = false;
    while (millis() - t0 < 4000) {
      st = WiFi.status();
      if (st == WL_CONNECTED) { connected = true; break; }
      delay(150);
    }
    if (connected) {
      ok = true; ip = WiFi.localIP().toString();
      attempts[n++] = "Attempt " + String(a+1) + ": OK";
      break;
    } else {
      attempts[n++] = "Attempt " + String(a+1) + ": status=" + String((int)WiFi.status());
    }
  }
  if (!ok) { WiFi.disconnect(true, true); delay(100); WiFi.mode(WIFI_AP); }
}

static void doMqttTest3(const String& host, uint16_t port, const String& user, const String& pass,
                        const String& clientId, bool& ok, String attempts[3], int& n) {
  ok = false; n = 0;
  for (int a=0; a<3; ++a) {
    PubSubClient tm(wifiClient);
    tm.setBufferSize(1024);
    tm.setServer(host.c_str(), port);
    String id = clientId + "-test-" + String(a+1);
    bool connected = user.length() ? tm.connect(id.c_str(), user.c_str(), pass.c_str())
                                   : tm.connect(id.c_str());
    if (connected) { ok = true; attempts[n++] = "Attempt " + String(a+1) + ": OK"; tm.disconnect(); break; }
    else { attempts[n++] = "Attempt " + String(a+1) + ": rc=" + String(tm.state()); delay(200); }
  }
}

void handleTestJson() {
  // Sticky cache from POST
  lastForm.valid = true;
  lastForm.wss   = server.hasArg("wss") ? server.arg("wss") : "";
  lastForm.wps   = server.hasArg("wps") ? server.arg("wps") : "";
  lastForm.mh    = server.hasArg("mh")  ? server.arg("mh")  : "";
  lastForm.mport = server.hasArg("mp")  ? server.arg("mp")  : "";
  lastForm.mu    = server.hasArg("mu")  ? server.arg("mu")  : "";
  lastForm.mx    = server.hasArg("mx")  ? server.arg("mx")  : "";
  lastForm.cid   = server.hasArg("cid") ? server.arg("cid") : "";
  lastForm.iv    = server.hasArg("iv")  ? server.arg("iv")  : "";
  lastForm.td    = server.hasArg("td")  ? server.arg("td")  : "";
  lastForm.ts    = server.hasArg("ts")  ? server.arg("ts")  : "";
  lastForm.dpfx  = server.hasArg("dpfx")? server.arg("dpfx"): "";
  lastForm.tdp   = server.hasArg("tdp") ? server.arg("tdp") : "";
  lastForm.ofs   = server.hasArg("ofs") ? server.arg("ofs") : "";
  lastForm.shape = server.hasArg("shape") ? server.arg("shape") : "";
  lastForm.cyl   = server.hasArg("cyl")   ? server.arg("cyl")   : "";
  lastForm.rlen  = server.hasArg("rlen")  ? server.arg("rlen")  : "";
  lastForm.rwid  = server.hasArg("rwid")  ? server.arg("rwid")  : "";
  lastForm.area  = server.hasArg("area")  ? server.arg("area")  : "";

  // Run tests (up to 3 attempts each)
  bool wifiOK=false, mqttOK=false;
  String wifiIp, wAttempts[3]; int wN=0;
  if (lastForm.wss.length()) doWiFiTest3(lastForm.wss, lastForm.wps, wifiOK, wifiIp, wAttempts, wN);
  else wAttempts[wN++] = "SSID empty";

  String mAttempts[3]; int mN=0;
  if (wifiOK && lastForm.mh.length()) {
    uint16_t port = clampPort(lastForm.mport.length() ? lastForm.mport.toInt() : 1883);
    doMqttTest3(lastForm.mh, port, lastForm.mu, lastForm.mx, lastForm.cid, mqttOK, mAttempts, mN);
  } else if (!wifiOK) mAttempts[mN++] = "Skipped (Wi-Fi not connected)";
  else mAttempts[mN++] = "MQTT host empty";

  if (!wifiOK) { WiFi.mode(WIFI_AP); WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0)); }

  // Build JSON response
  String json = "{";
  json += "\"wifi\":{";
  json += "\"ok\":"; json += wifiOK ? "true" : "false"; json += ",";
  json += "\"ip\":\""; json += wifiIp; json += "\",";
  json += "\"attempts\":[";
  for (int i=0;i<wN;i++){ if(i) json += ","; String t=wAttempts[i]; jsonEscape(t); json += "\""+t+"\""; }
  json += "]},";
  json += "\"mqtt\":{";
  json += "\"ok\":"; json += mqttOK ? "true" : "false"; json += ",";
  json += "\"attempts\":[";
  for (int i=0;i<mN;i++){ if(i) json += ","; String t=mAttempts[i]; jsonEscape(t); json += "\""+t+"\""; }
  json += "]}}";
  server.send(200, "application/json", json);
}

void handleTestHtml() { handleTestJson(); }

void handleStatusJson() {
  Reading r = takeReading();
  String j = "{";
  if (WiFi.status() == WL_CONNECTED) {
    j += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    j += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  }
  if (r.distance_mm > 0) {
    j += "\"distance_mm\":" + String(r.distance_mm) + ",";
    j += "\"level_mm\":" + String(r.level_mm) + ",";
    j += "\"percent\":" + String(r.percent,1) + ",";
    if (r.liters >= 0) j += "\"liters\":" + String(r.liters,2) + ",";
  }
  j += "\"uptime_ms\":" + String(millis());
  j += "}";
  server.send(200, "application/json", j);
}

void handleRediscover() {
  if (!mqtt.connected()) { server.send(200, "text/plain", "MQTT not connected."); return; }
  publishHADiscovery();
  server.send(200, "text/plain", "Discovery re-published. Check HA → MQTT → Listen to topic: " + cfg.discoveryPrefix + "/#");
}

// ========= Save =========
static uint16_t clampPort(long p);
void handleSave() {
  lastForm.valid = false;

  cfg.wifiSsid       = server.hasArg("wss") ? server.arg("wss") : "";
  cfg.wifiPass       = server.hasArg("wps") ? server.arg("wps") : "";
  cfg.mqttHost       = server.hasArg("mh")  ? server.arg("mh")  : "";
  { long p = server.hasArg("mp") ? server.arg("mp").toInt() : 1883; cfg.mqttPort = clampPort(p); }
  cfg.mqttUser       = server.hasArg("mu")  ? server.arg("mu")  : "";
  cfg.mqttPass       = server.hasArg("mx")  ? server.arg("mx")  : "";
  cfg.clientId       = server.hasArg("cid") ? server.arg("cid") : "tank1-esp32c3";
  { long iv = server.hasArg("iv") ? server.arg("iv").toInt() : (long)PUBLISH_MINUTES_DEFAULT; if (iv < 1) iv = PUBLISH_MINUTES_DEFAULT; cfg.publishMinutes = (uint32_t)iv; }
  cfg.topicData      = server.hasArg("td")  ? server.arg("td")  : "sensors/water/tank1";
  cfg.topicStatus    = server.hasArg("ts")  ? server.arg("ts")  : "sensors/water/tank1/status";
  cfg.discoveryPrefix= server.hasArg("dpfx")? server.arg("dpfx"): cfg.discoveryPrefix;

  cfg.tankDepth      = server.hasArg("tdp") ? server.arg("tdp").toInt() : TANK_DEPTH_MM_DEFAULT;
  cfg.sensorOffset   = server.hasArg("ofs") ? server.arg("ofs").toInt() : SENSOR_OFFSET_MM_DEFAULT;

  cfg.shape          = server.hasArg("shape") ? (uint8_t)server.arg("shape").toInt() : (uint8_t)SHAPE_CUSTOM;
  cfg.cylDiameterMm  = server.hasArg("cyl")   ? server.arg("cyl").toInt()   : 0;
  cfg.rectLenMm      = server.hasArg("rlen")  ? server.arg("rlen").toInt()  : 0;
  cfg.rectWidMm      = server.hasArg("rwid")  ? server.arg("rwid").toInt()  : 0;
  cfg.customAreaMm2  = server.hasArg("area")  ? server.arg("area").toInt()  : 0;

  cfg.configured     = true;
  saveConfig();

  String r = String(FPSTR(HTML_HDR)) + "<div class='card'><h3>Saved ✔</h3><p>Rebooting in 2 seconds…</p></div>" + FPSTR(HTML_FTR);
  server.send(200, "text/html", r);
  delay(2000);
  ESP.restart();
}

// ========= Portal (AP) =========
void startPortal() {
  uint8_t mac[6]; WiFi.softAPmacAddress(mac);
  char tail[5]; sprintf(tail, "%02X%02X", mac[4], mac[5]);
  apSsid = String("WaterNode-") + tail;

  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));

  const char* ap_pass = "WaterNode123";
  const int   channel = 6;
  const bool  hidden  = false;
  const int   maxconn = 4;

  bool up = WiFi.softAP(apSsid.c_str(), ap_pass, channel, hidden, maxconn);
  delay(100);

  Serial.printf("[PORTAL] AP %s on ch%d (WPA2)  IP=%s  up=%d\n",
    apSsid.c_str(), channel, WiFi.softAPIP().toString().c_str(), up);

  dns.start(53, "*", apIP);

  server.on("/",           HTTP_GET,  [](){ server.send(200, "text/html", htmlConfig()); });
  server.on("/config",     HTTP_GET,  [](){ server.send(200, "text/html", htmlConfig()); });
  server.on("/test",       HTTP_POST, handleTestHtml);
  server.on("/test.json",  HTTP_POST, handleTestJson);
  server.on("/save",       HTTP_POST, handleSave);
  server.on("/rediscover", HTTP_GET,  handleRediscover);

  server.onNotFound([&](){
    String host = server.hostHeader();
    if (!host.equals(apIP.toString())) {
      server.sendHeader("Location", String("http://") + apIP.toString(), true);
      server.send(302, "text/plain", "");
      return;
    }
    server.send(200, "text/html", htmlConfig());
  });

  server.on("/factory", HTTP_GET, [](){
    prefs.begin("waternode", false); prefs.clear(); prefs.end();
    server.send(200, "text/plain", "Preferences cleared. Rebooting..."); delay(300); ESP.restart();
  });

  server.begin();
  Serial.printf("[PORTAL] HTTP/DNS up. SSID: %s  Pass: %s\n", apSsid.c_str(), ap_pass);
}

// ========= Admin UI in STA mode =========
void startAdminUI_STA() {
  server.on("/",            HTTP_GET,  [](){ server.send(200, "text/html", htmlStatus()); });
  server.on("/config",      HTTP_GET,  [](){ server.send(200, "text/html", htmlConfig()); });
  server.on("/test",        HTTP_POST, handleTestHtml);
  server.on("/test.json",   HTTP_POST, handleTestJson);
  server.on("/save",        HTTP_POST, handleSave);
  server.on("/rediscover",  HTTP_GET,  handleRediscover);
  server.on("/reboot",      HTTP_GET,  [](){ server.send(200, "text/html", String(FPSTR(HTML_HDR)) + "<div class='card'><h3>Rebooting…</h3></div>" + FPSTR(HTML_FTR)); delay(500); ESP.restart(); });
  server.on("/status.json", HTTP_GET,  handleStatusJson);
  server.begin();
  logAdminURL();
}

// ========= Arduino =========
void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\n=== ESP32-C3 WaterNode Boot ===");

  loadConfig();

  // Force portal by holding BOOT at power-up
  pinMode(FORCE_PORTAL_BUTTON, INPUT_PULLUP);
  uint32_t t0 = millis();
  bool forcePortal = false;
  while (millis() - t0 < FORCE_PRESS_MS) { if (digitalRead(FORCE_PORTAL_BUTTON) == LOW) forcePortal = true; delay(5); }

  if (!cfg.configured || cfg.wifiSsid.length() == 0 || forcePortal) {
    Serial.println("[MODE] Config portal mode");
    startPortal();
    return;
  }

  Serial.println("[MODE] Normal run mode");
  wifiInitSTA();
  mqttInit();
  sonarInit();
  wifiKickOnce();
  startAdminUI_STA();
}

void loop() {
  // Portal mode
  if (!cfg.configured || WiFi.getMode() == WIFI_MODE_AP) {
    dns.processNextRequest();
    server.handleClient();
    delay(2);
    return;
  }

  // STA mode
  if (WiFi.status() != WL_CONNECTED) wifiKickOnce();
  else                               mqttKickOnce();

  if (mqtt.connected()) mqtt.loop();

  static uint32_t lastPub = 0;
  const uint32_t intervalMs = cfg.publishMinutes * 60UL * 1000UL;
  if (millis() - lastPub >= intervalMs) {
    measureAndPublish();
    lastPub = millis();
  }

  server.handleClient();
  delay(10);
}
