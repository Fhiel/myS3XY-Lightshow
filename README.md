# 🚗 myS3XY-Lightshow
![ESP32-C3](https://img.shields.io/badge/Hardware-ESP32--C3-blue?style=for-the-badge&logo=espressif)
![LED](https://img.shields.io/badge/LED-WS2812B%20%2F%20NeoPixel-green?style=for-the-badge&logo=lightbulb)
![License](https://img.shields.io/badge/License-MIT-yellow?style=for-the-badge)
[![GitHub Stars](https://img.shields.io/github/stars/Fhiel/myS3XY-Lightshow?style=for-the-badge&color=gold&logo=github)](https://github.com/Fhiel/myS3XY-Lightshow/stargazers)

An open-source companion for Tesla Light Shows – bring the spectacular synchronized light show experience to **any vehicle**, RC car, or custom LED setup.
---
<table style="border: none; border-collapse: collapse; width: 100%;">
  <tr style="border: none;">
    <td style="border: none; vertical-align: top; width: 50%;">
      <p align="center">
        <b>Video Demo (Shorts)</b><br>
        <a href="https://youtube.com/shorts/cibxwJfuThc">
          <img src="https://img.youtube.com/vi/cibxwJfuThc/0.jpg" width="340" style="border-radius:12px; border: 1px solid #333;">
        </a>
      </p>
    </td>
    <td style="border: none; vertical-align: top; width: 50%;">
      <p align="center">
        <b>Mobile Web App UI</b><br>
        <img src="https://github.com/user-attachments/assets/d5790efa-b21e-44a6-aad9-a65180e83c8a" width="210">
      </p>
    </td>
  </tr>
  <tr style="border: none;">
    <td style="border: none; vertical-align: top;">
      <p align="center">
        <b>ESP32-C3 Core</b><br>
        <img src="https://github.com/user-attachments/assets/85e6a9ff-aa47-4c61-8cdd-3b0d877cd703" width="200">
      </p>
    </td>
    <td style="border: none; vertical-align: top;">
      <p align="center">
        <b>WS2812B Hardware</b><br>
        <img src="https://github.com/user-attachments/assets/87c8ea1b-9e7f-4b0c-9a6f-ac13884cd955" width="180">
      </p>
    </td>
  </tr>
</table>


This project turns an inexpensive ESP32-C3 board with a small OLED display into a fully featured light show controller that:
- **Universal FSEQ:** Plays official Tesla multi-car FSEQ files (V1 Uncompressed).
- **Mobile Web App:** Tesla-style interface for selecting shows, hardware configs, and scheduling.
- **Smart Time Sync:** Automatically calculates UTC start times from your smartphone browser — no timezone settings required.
- **Offline Ready:** Since the app injects time directly from your browser, the system is fully functional in underground garages or remote locations without any internet access.
- **OLED Feedback:** Authentic Tesla-style countdown (MM:SS → Large Seconds → "GO!").
- **Flexible Mapping:** Map any LED to any Tesla channel via simple JSON files.
- **Wireless Updates:** Full OTA (Over-the-Air) support for firmware, shows, and configurations.
- **Troubleshooting Sparse Files:** If you use a professional show and your LEDs stay dark or show wrong colors, your FSEQ might have a different channel layout. Use the Channel Analyzer to identify which channels are active and update your 'config.json' accordingly.

---

## 🛠️ Hardware Requirements
- **Microcontroller:** ESP32-C3 (e.g., Seeed Studio XIAO or SuperMini).
- **Display:** SSD1306 128x64 I2C OLED.
- **LEDs:** WS2812B (NeoPixel) strip (tested up to 64 LEDs via USB power).
- **Total Cost:** Under 10 €.

---

## ⚠️ Technical Restrictions
To ensure stable performance and prevent memory issues on the ESP32-C3, the following limits apply to version 1.0.1:
- **Max. FSEQ File Size:** **1.5 MB** (due to LittleFS storage limits and file-seek performance).
- **Max. LED Count:** **100 LEDs** (buffer is optimized for stability; higher counts may impact frame rates).
- **Logical Channels:** Supports up to **512 channels** (Tesla standard mapping).
- **Storage:** Ensure at least **200 KB** of free space for system stability during playback.

---

## 📂 Configuration Guide (Custom LED Layouts)
The system uses **Absolute Mapping**. You map your physical LEDs directly to the global Tesla channel map. The controller automatically handles the colors based on the IDs you choose..
**Core Concepts:**
- **Smart Color Logic:** The controller handles the colors for you:
  - **Amber:** IDs 139, 142, 339, 342 (Indicators).
  - **Red:** IDs 300 - 399 (Tail, Brake, Rear Fog).
  - **White/Blue:** IDs 0 - 299 (Main Beams, Matrix Animations, Sig).
- **Flexible Setup:** You can create `Front_only`, `Rear_only`, or `Full_Car` setups just by listing the respective IDs in your JSON.
- **Sequential Freedom:** You can mix Front and Rear LEDs on a single strip in any order.
- **No Comments:** JSON files must not contain any comments (`//` or `/* */`). Use the structure below exactly.
- **Channel 9999:** Use this for "dead" LEDs or spacing on your strip.
> [!IMPORTANT]
> **Filename Convention:** Configuration files must start with `config_` and end with `.json` (e.g., `config_cybertruck_front.json`) to be recognized by the system.
> **Mapping:** Ensure your JSON defines the correct Tesla-specific channels (e.g., 139 for Indicators). The controller maps these 1:1 to your physical LED sequence.

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


### Example `config_all_25.json` (The "All-In-One" 25-LED Setup)
This compact configuration maps every essential Tesla light function exactly once. It’s perfect for RC cars or small desktop models.
```json
{
  "name": "RC_S3XY_Compact_25",
  "channel_offset": 0,
  "max_brightness": 128,
  "max_milliamps": 1200,
  "leds": [
    {"channel": 139}, {"channel": 164}, {"channel": 151}, {"channel": 152},
    {"channel": 153}, {"channel": 154}, {"channel": 189}, {"channel": 192},
    {"channel": 155}, {"channel": 158}, {"channel": 159}, {"channel": 160},
    {"channel": 165}, {"channel": 142}, 
    {"channel": 339}, {"channel": 364}, {"channel": 365}, {"channel": 370}, 
    {"channel": 371}, {"channel": 390}, {"channel": 391}, {"channel": 392}, 
    {"channel": 184}, {"channel": 380}, {"channel": 342}
  ]
}
```
*Note: Use 9999 for "dead" LEDs or spacing on your strip.*

---

## 📱 Web Interface Manual
- **Schedule Show:** Select your .fseq file and a start time. The "START COUNTDOWN" button sends all data to the ESP32. The system uses Client-Side Time Synchronization to ensure perfect alignment between your smartphone and the controller, regardless of your local timezone.
- **NOW Button:** Immediate launch for testing.
- **Advanced Config:** Switch between hardware layouts (e.g., "Front-only" to "Full-64-LEDs") on the fly.
- **Storage Explorer:**
  - **Upload:** Drag & drop new .fseq or .json files via your browser. 
  - **Delete:** Manage your storage space wirelessly.
- **OTA Portal:** Dedicated link for wireless firmware updates.

---

## 🚀 Getting Started

>[!TIP] 
> Easy Installation: If you are a new user or have a fresh ESP32-C3 board, you don't need to set up a development environment.
> 1. Download the `full_install_v1.x.x.bin` from the Latest Release.
> 2. Open the **[ESPHome Web Flasher](https://web.esphome.io/)**
> 3. Use **CONNECT** and **INSTALL** to flash the file in one step (Firmware + Web-UI).
> 4. For future updates, simply use the OTA Update button within the Web App and upload the smaller `firmware.bin`.

#### 1. Prepare your FSEQ Files
The controller is optimized for **FSEQ V1 (Uncompressed).**
- **Size Limit:** Keep files under 1.5 MB for best stability. If your file is too large, see our **[Optimization Guide](#-pro-tip-optimize-large-fseq-files)**.
- **Official Shows:** Professional shows often use a "Sparse" format. Our engine uses Stride Emulation to play these perfectly.
- **Avoid V2 Compressed:** If your file is a .fseq V2 (Zstd), you must **[re-export it in xLights](#-pro-tip-optimize-large-fseq-files)** as V1 Uncompressed.

#### 2. Connection & Best Practice (Outdoor Setup)
Since light shows usually happen outdoors, the controller is pre-configured to connect to a mobile hotspot. This allows the ESP32 and your smartphone to communicate on the same network for **millisecond-precise browser-based time synchronization**.

**Pre-configured Credentials:**
- **SSID:** `LIGHTSHOW`
- **Password:** `mys3xyls`
- **Hostname:** `mys3xy.local`

**How to find the Web App:**
1. **Check the OLED Display:** Once connected, the ESP32 will show its assigned **IP Address** (e.g., `192.168.178.42`) and the hostname 'mys3xy.local' directly on the screen.
2. **Open Browser:** Type this IP address into your mobile browser's address bar or try `mys3xy.local` if your device supports this.
3. **Android Users:** If `mys3xy.local` does not work, please refer to the **[Android Connectivity Tip](#-android--connectivity)**.

#### 3. Launching Your First Show
1. **Inital Upload:** 
  * Scroll down to the Storage Explorer at the bottom of the Web App.
  * Upload your optimized .fseq file.
  * Upload your .json hardware mapping file (see Hardware Mapping section for examples).
2. **Select & Configure:**
  * Choose your uploaded show file and the correct hardware config from the dropdown menus.
  * The status pill will update to "READY (YourConfig.json)".
3. **Trigger the Magic:** 
  * **NOW** Tap for an immediate start.
  * **START COUNTDOWN** Select a time (e.g., 2 minutes ahead) and tap to sync with other cars. The OLED will show the official Tesla style countdown sequence (MM:SS → Large Seconds → "GO!").

---

## 🔍 Using the Channel Analyzer
If you are using a custom or modified FSEQ file and don't know the channel mapping, enable Channel Analyzer Mode in the Web UI.
1. **Enable Scan:** Check the "Enable Channel Analyzer" box before starting a show.
2. **Live Feedback:** Your first 32 LEDs will act as a "VU-Meter," glowing brighter as data passes through channels 0–31.
3. **Serial Report:** Connect your ESP32 to a PC and open the Serial Monitor (115200 baud). The controller will print a report every 100 frames showing every channel that has registered a brightness level above 50.
4. **Identification:** Watch the car (or xLights) and the Serial Monitor simultaneously. If the left blinker flashes in the video, look for the channel number in the monitor that spikes at the same moment.

---

> [!TIP]
> **Power management:** For setups with more than 64 LEDs, use an external 5V power source. The ESP32's onboard regulator is not designed for high-current LED strips.
---
## 📱 Android & Connectivity
> [!TIP]
>  **mDNS Issues:** Android often fails to resolve .local hostnames (mDNS). If you cannot reach mys3xy.local:
>  1. Go to your phone's **Mobile Hotspot** settings.
>  2. Check **Connected Devices** for an entry like `esp32c3-xxxxxx`.
>  3. Tap the **Info (i)** icon to see the IP.
---
## 💡 Pro-Tip: Optimize large FSEQ files
> [!TIP]
> **Saving Space:** If your favorite show is larger than 1.5 MB, you can easily shrink it using xLights without losing any visible quality:
>  1. Open **xLights** and load your sequence.
>  2. Go to **Setup** and ensure your "Network" is set to the channels you actually use (e.g., 512).
>  3. In the **Controller** tab, make sure "Full xLights Support" is NOT active for unnecessary channels.
>  4. Go to **File -> Render All** to recalculate the frames.
>  5. Export as **FSEQ Version 1 (V1)**. Note: V2 files are often larger due to compression headers that the ESP32 doesn't need.
>  6. This typically reduces file size by **50-70%**, making even long shows fit perfectly on your S3XY-Lightshow Controller.

---

## ⚖️ License & Credits

- **Core Logic:** Deeply inspired by the [official Tesla Motors Light Show](https://github.com/teslamotors/light-show) repository. We use the same channel-mapping standards to ensure compatibility with existing `.fseq` shows.
- **Sync & FSEQ Architecture:** Special thanks to the GitHub community and contributors like **[Cryptkeeper](https://github.com/cryptkeeper)** (for the essential FSEQ parsing logic) and **[Simon-Wh](https://github.com/simon-wh)** (for foundational work on synchronized Lightshow playback).
- **AI-Powered Development:** This project was co-developed and optimized with **Google Gemini**. From fixing critical memory leaks to designing the Tesla-style countdown UI, Gemini served as the digital lead engineer behind v1.0.1.
- **Frameworks & Libraries:**
  - [FastLED](https://fastled.io/) - Powering the high-speed LED animations.
  - [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer) - Responsive mobile dashboard.
  - [ElegantOTA](https://github.com/ayushsharma82/ElegantOTA) - Effortless wireless firmware updates.
  - [ArduinoJson](https://arduinojson.org/) - High-performance configuration handling.
---

## Upcoming: 
🎵 Local Audio Sync Play the matching soundtrack directly from your smartphone's local storage, perfectly synced with the ESP32 lightshow. Use your phone's Bluetooth to connect to external speakers for the full experience.

---
# Let’s make every car part of the show! 🚗💡
