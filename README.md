# 🚗 myS3XY-Lightshow
![ESP32-C3](https://img.shields.io/badge/Hardware-ESP32--C3-blue?style=for-the-badge&logo=espressif)
![LED](https://img.shields.io/badge/LED-WS2812B%20%2F%20NeoPixel-green?style=for-the-badge&logo=lightbulb)
![License](https://img.shields.io/badge/License-MIT-yellow?style=for-the-badge)
[![GitHub Stars](https://img.shields.io/github/stars/Fhiel/myS3XY-Lightshow?style=for-the-badge&color=gold&logo=github)](https://github.com/Fhiel/myS3XY-Lightshow/stargazers)

An open-source companion for Tesla Light Shows – bring the spectacular synchronized light show experience to **any vehicle**, RC car, or custom LED setup.
---
<img width="190" height="321" alt="image" src="https://github.com/user-attachments/assets/d5790efa-b21e-44a6-aad9-a65180e83c8a" /> <img width="350" height="300" alt="image" src="https://github.com/user-attachments/assets/85e6a9ff-aa47-4c61-8cdd-3b0d877cd703" />
<img width="300" height="250" alt="image" src="https://github.com/user-attachments/assets/87c8ea1b-9e7f-4b0c-9a6f-ac13884cd955" />




This project turns an inexpensive ESP32-C3 board with a small OLED display into a fully featured light show controller that:
- **NTP-Synced:** Perfectly synchronizes with real Teslas using Network Time Protocol.
- **Universal FSEQ:** Plays official Tesla multi-car FSEQ files (V1 Uncompressed).
- **Mobile Web App:** Tesla-style interface for selecting shows, hardware configs, and scheduling.
- **Smart Time Sync:** Automatically calculates UTC start times from your smartphone browser—no timezone settings required.
- **OLED Feedback:** Authentic Tesla-style countdown (MM:SS → Large Seconds → "GO!").
- **Flexible Mapping:** Map any LED to any Tesla channel via simple JSON files.
- **Troubleshooting Sparse Files:** If you use a professional show and your LEDs stay dark or show wrong colors, your FSEQ might have a different channel layout. Use the Channel Analyzer to identify which channels are active and update your 'config.json' accordingly.
- **Wireless Updates:** Full OTA (Over-the-Air) support for firmware, shows, and configurations.

---

## 🛠️ Hardware Requirements
- **Microcontroller:** ESP32-C3 (e.g., Seeed Studio XIAO or SuperMini).
- **Display:** SSD1306 128x64 I2C OLED.
- **LEDs:** WS2812B (NeoPixel) strip (tested up to 64 LEDs via USB power).
- **Total Cost:** Under 10 €.

---

## 📂 Configuration Guide (Custom LED Layouts)
The system uses **Absolute Mapping**. You map your physical LEDs directly to the global Tesla channel map. The controller automatically handles the colors based on the IDs you choose..
**Core Concepts:**
- **Smart Color Logic:** The controller handles the colors for you:
  - **Amber:** IDs 139, 142, 339, 342 (Indicators).
  - **Red:** IDs 300 - 399 (Tail, Brake, Rear Fog).
  - **White/Blue:** IDs 0 - 299 (Main Beams, Matrix Animations, Sig).
- **Flexible Setup:** You can create "Front-only", "Rear-only", or "Full-Car" setups just by listing the respective IDs in your JSON.
- **Sequential Freedom:** You can mix Front and Rear LEDs on a single strip in any order.
- **No Comments:** JSON files must not contain any comments (// or /* */). Use the structure below exactly.
- **Channel 9999:** Use this for "dead" LEDs or spacing on your strip.
> [!IMPORTANT]
>  Ensure the number of LEDs defined in your config.json matches your physical LED strip length. The controller uses the first half of your JSON entries for front animations and the rest for the rear.

### Common Tesla Channel Map (Absolute)
| Section |Function | FSEQ Channel | Expected Color |
| :--- | :--- | :---| :--- |
| **FRONT** | Indicators Left / Right | 139 (L), 142 (R) | Amber |
| **FRONT** | **Matrix Effects (Animations) ** | 151 - 160 | Blue/White |
| **FRONT**| **Main Beams / Fog / Sig ** | 184 - 192 | Bright White |
| **REAR** | **Tail / Brake Lights** | 164 - 183 | Red |
| **REAR** | **Rear Indicators** | 339 (L), 342 (R) | Amber |
| **REAR** | **License / Reverse Lights** | 184, 390 - 391 | White |
| **REAR** | **High Brake (Center)** | 392 | Red |


### Example `config_all_28.json`(The "All-In-One" 25-LED Setup)
This compact configuration maps every essential Tesla light function exactly once. It’s perfect for RC cars or small desktop models.
```json
{
  "name": "RC_S3XY_Compact_28",
  "channel_offset": 0,
  "max_brightness": 128,
  "max_milliamps": 1200,
  "leds": [
    {"channel": 139}, {"channel": 164}, {"channel": 151}, {"channel": 152},
    {"channel": 153}, {"channel": 154}, {"channel": 189}, {"channel": 192},
    {"channel": 155}, {"channel": 158}, {"channel": 159}, {"channel": 160},
    {"channel": 165}, {"channel": 142}, {"channel": 339}, {"channel": 364},
    {"channel": 365}, {"channel": 370}, {"channel": 371}, {"channel": 390},
    {"channel": 391}, {"channel": 392}, {"channel": 184}, {"channel": 184},
    {"channel": 184}, {"channel": 184}, {"channel": 380}, {"channel": 342}
  ]
}
```
Note: Use 9999 for "dead" LEDs or spacing on your strip. 

📱 Web Interface Manual
- **Schedule Show: Select your .fseq file and a start time. The "START COUNTDOWN" button sends all data to the ESP32. The system uses Client-Side Time Synchronization to ensure perfect alignment between your smartphone and the controller, regardless of your local timezone.
- **NOW Button:** Immediate launch for testing.
- **Advanced Config:** Switch between hardware layouts (e.g., "Front-only" to "Full-64-LEDs") on the fly.
- **Storage Explorer:** - Upload: Drag & drop new .fseq or .json files via your browser.
- **Delete:** Manage your storage space wirelessly.
- **OTA Portal:** Dedicated link for wireless firmware updates.

---

## 🔍 Using the Channel Analyzer
If you are using a custom or modified FSEQ file and don't know the channel mapping, enable Channel Analyzer Mode in the Web UI.
1. **Enable Scan:** Check the "Enable Channel Analyzer" box before starting a show.
2. **Live Feedback:** Your first 32 LEDs will act as a "VU-Meter," glowing brighter as data passes through channels 0–31.
3. **Serial Report:** Connect your ESP32 to a PC and open the Serial Monitor (115200 baud). The controller will print a report every 100 frames showing every channel that has registered a brightness level above 50.
4. **Identification:** Watch the car (or xLights) and the Serial Monitor simultaneously. If the left blinker flashes in the video, look for the channel number in the monitor that spikes at the same moment.

---

## 🚀 Getting Started

#### 1. Prepare your FSEQ Files
The controller is optimized for **FSEQ V1 (Uncompressed).**
- **Size Limit:** Keep files under 1.5 MB for best stability on LittleFS.
- **Official Shows:** Professional shows (like xLightshows.io) often use a "Sparse" format. Our v1.0.0 engine uses Stride Emulation to play these files perfectly for their full duration.
- **Avoid V2 Compressed:** If your file is a .fseq V2 (Zstd), you must re-export it in xLights as V1 Uncompressed.

#### 2. Connection & Best Practice (Outdoor Setup)
Since light shows usually happen outdoors, the controller is pre-configured to connect to a mobile hotspot. This allows the ESP32 to fetch the precise NTP time for synchronization with real Teslas.

**Pre-configured Credentials:**
- **SSID:** `LIGHTSHOW`
- **Password:** `mys3xyls`
- **Hostname:** 'mys3xy.local'

**How to find the Web App:**
1. **Check the OLED Display:** Once connected, the ESP32 will show its assigned **IP Address** (e.g., `192.168.178.42`) and the hostname 'mys3xy.local' directly on the screen.
2. **Open Browser:** Type this IP address into your mobile browser's address bar or try 'mys3xy.local' if your device supports this.

**Pro-Tip for Android Users:**
Android doesn't support local hostnames (mDNS) well. If you miss the IP on the display:
1. Go to your phone's **Mobile Hotspot** settings.
2. Check **Connected Devices** for an entry like `esp32c3-xxxxxx`.
3. Tap the **Info (i)** icon to see the IP.

#### 3. Launching Your First Show
1. **Inital Upload:** 
  * Scroll down to the Storage Explorer at the bottom of the Web App.
  * Upload your optimized .fseq file.
  * Upload your .json hardware mapping file (see Hardware Mapping section for examples).
2. **Select & Configure:**
  * Choose your uploaded show file and the correct hardware config from the dropdown menus.
  * The status pill will update to "READY (YourConfig.json)".
3. **Trigger the Magic:** 
  * **NOW:** Tap for an immediate start.
  * **START COUNTDOWN:** Select a time (e.g., 2 minutes ahead) and tap to sync with other cars. The OLED will show the official Tesla style countdown sequence (MM:SS → Large Seconds → "GO!").

---

💡 Tricks and Tips
- **Power supply:** For >64 LEDs, use an external 5V power supply. Do not power long strips solely through the ESP32.
- **International Sync:** The web app uses UTC timestamps. It works in any timezone without manual adjustment.

⚖️ License & Credits
- **Logic:** Inspired by the official Tesla Motors GitHub.
- **Libraries:** FastLED, ESPAsyncWebServer, ArduinoJson, ElegantOTA.

Let’s make every car part of the show! 🚗💡
