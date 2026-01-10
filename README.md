# 🚗 myS3XY-Lightshow

An open-source companion for Tesla Light Shows – bring the spectacular synchronized light show experience to **any vehicle**, RC car, or custom LED setup.



This project turns an inexpensive ESP32-C3 board with a small OLED display into a fully featured light show controller that:
- **NTP-Synced:** Perfectly synchronizes with real Teslas using Network Time Protocol.
- **Universal FSEQ:** Plays official Tesla multi-car FSEQ files (V1 Uncompressed).
- **Mobile Web App:** Tesla-style interface for selecting shows, hardware configs, and scheduling.
- **Smart Time Sync:** Automatically calculates UTC start times from your smartphone browser—no timezone settings required.
- **OLED Feedback:** Authentic Tesla-style countdown (MM:SS → Large Seconds → "GO!").
- **Flexible Mapping:** Map any LED to any Tesla channel via simple JSON files.
- **Wireless Updates:** Full OTA (Over-the-Air) support for firmware, shows, and configurations.

---

## 🛠️ Hardware Requirements
- **Microcontroller:** ESP32-C3 (e.g., Seeed Studio XIAO or SuperMini).
- **Display:** SSD1306 128x64 I2C OLED.
- **LEDs:** WS2812B (NeoPixel) strip (tested up to 64 LEDs via USB power).
- **Total Cost:** Under 10 €.

---

## 📂 Configuration Guide (Custom LED Layouts)

The system uses **Absolute Mapping (Offset 0)**. This means you map your physical LEDs directly to the global Tesla channel map found in the FSEQ file.

### Common Tesla Channel Map (Absolute)
| Function | Physical Channel | Expected Color Logic |
| :--- | :--- | :--- |
| **Front Indicators** | 139 (L), 142 (R) | Amber |
| **Signature / DRL** | 151 - 160 | Cold White |
| **Main Beams / Fog** | 184 - 192 | Bright White |
| **Tail / Brake Lights** | 164 - 183 | Red |
| **Rear Indicators** | 339 (L), 342 (R) | Amber |
| **Reverse Lights** | 390 - 392 | White |



### Example `config.json`
```json
{
  "name": "Tesla_64_Symmetric",
  "data_pin": 10,
  "max_brightness": 128,
  "max_milliamps": 2500,
  "channel_offset": 0,
  "leds": [
    {"channel": 139},
    {"channel": 164},
    {"channel": 9999}
  ]
}
```
Note: Use 9999 for "dead" LEDs or spacing on your strip.

📱 Web Interface Manual
Schedule Show: Select your .fseq file and a start time. The "START COUNTDOWN" button uses your phone's clock to sync the ESP32 globally.

NOW Button: Immediate launch for testing.

Advanced Config: Switch between hardware layouts (e.g., "Front-only" to "Full-64-LEDs") on the fly.

Storage Explorer: - Upload: Drag & drop new .fseq or .json files via your browser.

Delete: Manage your storage space wirelessly.

OTA Portal: Dedicated link for wireless firmware updates.

---

## 🚀 Getting Started

#### 1. Optimize your FSEQ Files (The "Tesla Shrink")
Official shows (like the Xmas 2025 show) are often >1.4 MB. To save memory and ensure smooth playback on the ESP32-C3:
1. Open the `.fseq` show in **xLights**.
2. Export as **FSEQ V1 (Uncompressed)**. *Note: ESP32 cannot handle Zstd-compressed V2 files in real-time.*
3. Restrict the channel range (e.g., 0-512) to keep the file size <100 KB.

#### 2. Connection & Best Practice (Outdoor Setup)
Since light shows usually happen outdoors, the controller is pre-configured to connect to a mobile hotspot. This allows the ESP32 to fetch the precise NTP time for synchronization with real Teslas.

**Pre-configured Credentials:**
* **SSID:** `LIGHTSHOW`
* **Password:** `mys3xyls`

**How to find the Web App:**
1. **Check the OLED Display:** Once connected, the ESP32 will show its assigned **IP Address** (e.g., `192.168.178.42`) directly on the screen.
2. **Open Browser:** Type this IP address into your mobile browser's address bar.



**Pro-Tip for Android Users:**
Android doesn't support local hostnames (mDNS) well. If you miss the IP on the display:
* Go to your phone's **Mobile Hotspot** settings.
* Check **Connected Devices** for an entry like `esp32c3-xxxxxx`.
* Tap the **Info (i)** icon to see the IP.

#### 3. Launching Your First Show
1. **Upload:** Use the **Storage Explorer** at the bottom of the Web App to upload your optimized `.fseq` file and your `.json` hardware config.
2. **Select & Configure:** Choose your show file and the correct hardware config from the dropdown menus.
3. **Trigger:** - Tap **NOW** for an immediate start.
   - Or select a time and tap **START COUNTDOWN** to sync with other cars. The OLED will show the official Tesla countdown (MM:SS → Large Seconds → "GO!").

---

💡 Tricks and Tips
Power: For >64 LEDs, use an external 5V power supply. Do not power long strips solely through the ESP32.

International Sync: The web app uses UTC timestamps. It works in any timezone without manual adjustment.

⚖️ License & Credits
Logic: Inspired by the official Tesla Motors GitHub.

Libraries: FastLED, ESPAsyncWebServer, ArduinoJson, ElegantOTA.

Let’s make every car part of the show! 🚗💡
