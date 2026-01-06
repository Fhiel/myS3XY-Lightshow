# myS3XY-Lightshow

An open-source companion for Tesla Light Shows – bring the spectacular synchronized light show experience to **any vehicle**, RC car, or custom LED setup.

This project turns an inexpensive ESP32-C3 board with a small OLED display into a fully featured light show controller that:
- Plays official Tesla multi-car FSEQ files (Trans-Siberian Orchestra, Sandstorm, etc.)
- Perfectly synchronizes with real Teslas using NTP time (just like the original)
- Offers a Tesla-style web app for selecting configurations and scheduling start time (up to 10 minutes ahead)
- Displays the authentic Tesla countdown (MM:SS → large seconds → "GO!")
- Supports flexible LED mapping via simple JSON config files (front only, rear only, full vehicle, custom layouts)
- Allows wireless over-the-air updates for new shows and configs (no cable needed after initial setup)

Perfect for:
- Joining a real Tesla light show with a second (non-Tesla) car
- Adding extra lights to your Tesla
- Building amazing RC car light shows
- Any creative LED project that wants Tesla-level synchronization

### Features
- NTP-based precise timing (same as Tesla)
- Tesla-accurate countdown on built-in OLED
- Web interface for show selection and scheduling
- ElegantOTA for wireless firmware & file updates
- Configurable LED mapping (front/rear/custom)
- Works with single or multiple FSEQ files

Hardware: Cheap ESP32-C3 + WS2812B LEDs – total cost under 10 €.

Let’s make every car part of the show! 🚗💡
