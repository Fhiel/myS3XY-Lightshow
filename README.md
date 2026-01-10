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