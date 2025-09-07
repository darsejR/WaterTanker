# ESP32-C3 WaterNode

Ultrasonic water tank monitor with captive portal, on-device admin UI, MQTT publishing, and Home Assistant auto-discovery.  
Built for the **ESP32-C3 Dev Module**.

---

## ✨ Features

- **First-boot captive portal** (`WaterNode-XXXX`) at `http://192.168.4.1/` for Wi-Fi & MQTT setup  
- **Admin UI** at `http://<device-ip>/` with live status, configuration, and tools  
- **3-attempt Wi-Fi & MQTT tester** with modal feedback  
- **MQTT JSON telemetry**: `distance_mm`, `level_mm`, `percent`, `liters`, `rssi`  
- **Retained availability** (`online`/`offline`) via LWT  
- **Home Assistant MQTT Discovery** with configurable `discoveryPrefix`  
- **Tank geometry support**: cylindrical, rectangular, or custom cross-section area  
- **Persistent settings** stored in flash (Preferences API)  
- **Endpoints**: `/status.json`, `/rediscover`, `/reboot`, `/factory`  

---

## 🛠️ Hardware

- **Board**: ESP32-C3 Dev Module  
- **Sensor**: HC-SR04 (TRIG=GPIO4, ECHO=GPIO5 **via 10k/15k divider**)  
- **Force-portal button**: GPIO9 held LOW ≥2s at boot  
- **Power**: USB-C 5V  

---

## 🚀 Getting Started

### 1. Flash Firmware
- Open Arduino IDE or PlatformIO  
- Select **ESP32C3 Dev Module**  
- Enable `USB CDC On Boot`  
- Upload `WaterNode.ino`  

### 2. First Boot
- Device starts in **AP portal mode**  
- Connect to Wi-Fi `WaterNode-XXXX` (pass: `WaterNode123`)  
- Open [http://192.168.4.1/](http://192.168.4.1/)  
- Enter Wi-Fi + MQTT broker details, tank dimensions, and save  

### 3. Admin UI
- Once connected to Wi-Fi, visit `http://<device-ip>/`  
- Features:  
  - Live sensor readings (distance, level, %, liters)  
  - Config page with MQTT, HA, and tank geometry  
  - Buttons: test Wi-Fi/MQTT, re-publish HA discovery, save & reboot  

### 4. MQTT Topics
- **Status (retained)**: `sensors/water/tank1/status` → `online` / `offline`  
- **Data (JSON)**: `sensors/water/tank1`  

Example payload:
json
{
  "distance_mm": 1234,
  "level_mm": 980,
  "percent": 65.3,
  "liters": 450.12,
  "rssi": -58
}
5. Home Assistant Integration
Enable MQTT integration in HA

Discovery prefix defaults to homeassistant

Entities created:

Water Level (mm)

Water Level (%)

Water Volume (L)

Sonar Distance (mm)

Wi-Fi RSSI

Run /rediscover if entities don’t appear.

🔧 Useful Endpoints
/ – Status page

/config – Configuration form

/status.json – Raw JSON state

/rediscover – Force HA discovery publish

/reboot – Restart device

/factory – Clear preferences & reset

📐 Tank Geometry
Cylinder – enter diameter (mm)

Rectangular – enter length × width (mm)

Custom – enter cross-section area (mm²)

Liters = area × water level ÷ 1,000,000

⚠️ Notes
Keep sensor dry; condensation can cause false reads

Always use resistor divider on ECHO (HC-SR04 outputs 5V)

For long-term reliability, increase publish interval to reduce broker load

📄 License
MIT License

pgsql
Copy code

Do you want me to also add a **Quick Deploy / Binary Releases** section so people can flash it without compiling? That would make it friendlier for non-developers.





You said:
yes please


ChatGPT said:
Perfect — here’s the README.md updated with a Quick Deploy / Binary Releases section included:

markdown
Copy code
# ESP32-C3 WaterNode

Ultrasonic water tank monitor with captive portal, on-device admin UI, MQTT publishing, and Home Assistant auto-discovery.  
Built for the **ESP32-C3 Dev Module**.

---

## ✨ Features

- **First-boot captive portal** (`WaterNode-XXXX`) at `http://192.168.4.1/` for Wi-Fi & MQTT setup  
- **Admin UI** at `http://<device-ip>/` with live status, configuration, and tools  
- **3-attempt Wi-Fi & MQTT tester** with modal feedback  
- **MQTT JSON telemetry**: `distance_mm`, `level_mm`, `percent`, `liters`, `rssi`  
- **Retained availability** (`online`/`offline`) via LWT  
- **Home Assistant MQTT Discovery** with configurable `discoveryPrefix`  
- **Tank geometry support**: cylindrical, rectangular, or custom cross-section area  
- **Persistent settings** stored in flash (Preferences API)  
- **Endpoints**: `/status.json`, `/rediscover`, `/reboot`, `/factory`  

---

## 🛠️ Hardware

- **Board**: ESP32-C3 Dev Module  
- **Sensor**: HC-SR04 (TRIG=GPIO4, ECHO=GPIO5 **via 10k/15k divider**)  
- **Force-portal button**: GPIO9 held LOW ≥2s at boot  
- **Power**: USB-C 5V  

---

## 🚀 Getting Started

### Option 1: Quick Deploy (Prebuilt Binary)
1. Download the latest `.bin` firmware from the [Releases](../../releases) page.  
2. Open [ESP Web Flasher](https://esphome.github.io/esp-web-tools/) in Chrome/Edge.  
3. Connect your ESP32-C3 via USB-C.  
4. Select the `.bin` and flash it.  
5. After reboot, connect to the AP `WaterNode-XXXX` (password: `WaterNode123`).  
6. Open [http://192.168.4.1/](http://192.168.4.1/) and configure Wi-Fi + MQTT.

### Option 2: Build from Source
- Open Arduino IDE or PlatformIO  
- Select **ESP32C3 Dev Module**  
- Enable `USB CDC On Boot`  
- Install library **PubSubClient (Nick O’Leary)**  
- Upload the sketch  

---

## 🌐 Admin UI

Once on Wi-Fi, open `http://<device-ip>/` for:  
- Live readings (distance, level, %, liters)  
- Configuration form  
- Tools: test Wi-Fi/MQTT, re-publish HA discovery, save & reboot  

---

## 📡 MQTT Topics

- **Status (retained)**: `sensors/water/tank1/status` → `online` / `offline`  
- **Data (JSON)**: `sensors/water/tank1`  

Example payload:
json
{
  "distance_mm": 1234,
  "level_mm": 980,
  "percent": 65.3,
  "liters": 450.12,
  "rssi": -58
}
🏠 Home Assistant Integration
Enable MQTT integration in HA

Discovery prefix defaults to homeassistant

Entities created:

Water Level (mm)

Water Level (%)

Water Volume (L)

Sonar Distance (mm)

Wi-Fi RSSI

If entities don’t appear, visit:

arduino
Copy code
http://<device-ip>/rediscover
🔧 Useful Endpoints
/ – Status page

/config – Configuration form

/status.json – Raw JSON state

/rediscover – Force HA discovery publish

/reboot – Restart device

/factory – Clear preferences & reset

📐 Tank Geometry
Cylinder – enter diameter (mm)

Rectangular – enter length × width (mm)

Custom – enter cross-section area (mm²)

Liters = area × water level ÷ 1,000,000

⚠️ Notes
Keep sensor dry; condensation can cause false reads

Always use resistor divider on ECHO (HC-SR04 outputs 5V)

For long-term reliability, increase publish interval to reduce broker load

📄 License
MIT License
