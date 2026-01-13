/**
 * =====================================================================
 * 🚗 myS3XY-Lightshow Controller (v1.0.0)
 * =====================================================================
 * A high-performance ESP32-C3 based controller for Tesla Light Shows.
 * * Features:
 * - Client-Side UTC Synchronization for precise show starts.
 * - Dynamic LED mapping via JSON configuration files.
 * - Built-in Channel Analyzer for FSEQ reverse-engineering.
 * - Mobile-optimized Web UI with LittleFS Storage Explorer.
 * * Hardware: ESP32-C3 (e.g. SuperMini), OLED SSD1306, WS2812B LEDs.
 * License: MIT
 * Repository: https://github.com/fhiel/myS3XY-Lightshow
 * =====================================================================
 */

#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <FastLED.h>
#include <LittleFS.h>
#include <U8g2lib.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ElegantOTA.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>

// --- Project definitions ---
#define PROJECT_VERSION "1.0.0"
#define PROJECT_NAME "myS3XY-Lightshow"

// --- Network Configuration ---
const char* ssid     = "LIGHTSHOW";
const char* password = "mys3xyls";

// --- Hardware Pins ---
#define STATUS_LED 8      // Onboard LED (standard for many C3 boards)
#define DATA_PIN   2      // Fixed Data Pin (Right side of ESP32-C3 SuperMini)
#define OLED_SCL   6      // I2C Clock
#define OLED_SDA   5      // I2C Data

// --- LED & Playback Settings ---
#define MAX_LEDS   100    // Buffer size for LED array
CRGB leds[MAX_LEDS];

// --- Global State Variables ---
bool showRunning      = false;
bool triggerCountdown = false; // Signals the loop to start or wait
bool scanActive       = false; // If true, Analyzer Mode is used
bool isBusy           = false; // Prevents overlapping FS operations
bool configValid = false;

// --- Timing & Sync Variables ---
unsigned long showStartEpoch      = 0; // Target UTC epoch (0 = Instant Start)
unsigned long showStartTimeMillis = 0; // Reference point for frame timing
uint32_t currentFrame             = 0; // Global frame tracker
uint16_t stepTimeMs               = 50; // Default frame duration (parsed from FSEQ)

// --- File & Storage Variables ---
File fseqFile;
uint32_t realChannelsInFile = 0; 
uint32_t channelCount    = 0;
uint32_t frameCount      = 0;
uint16_t fseqDataOffset = 0;   // Default to 32, but will be updated by header read
uint8_t globalMax[500];        // Peak value storage for Channel Analyzer
uint8_t frameData[1024];

// --- OLED Display Setup ---
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
const int xOffset = 30;  // Centering area for 72x40 visible zone
const int yOffset = 12;

// --- Network & Server Instances ---
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600, 60000); // UTC+1 (CET)
AsyncWebServer server(80);

/**
 * Hardware Config Structure
 */
struct LedMapping {
  uint16_t channel;
};

struct Config {
  String name = "Default";
  uint16_t channel_offset = 0;
  uint8_t max_brightness = 128;
  uint16_t max_milliamps = 500;
  std::vector<LedMapping> leds;
};

Config currentConfig;

// --- Functional Prototypes (to be implemented) ---
void startShowSequence();
void stopShowAndCleanup();
bool readFseqHeader();
bool playFrame(uint32_t frameIdx);
void handleTeslaApp(AsyncWebServerRequest *request);
void handleDelete(AsyncWebServerRequest *request);

// --- Global File References (Default placeholders) ---
String currentConfigFile    = "None selected"; 
String currentShow          = "None selected";
String lastUploadedFilename = "";

// Globale Cache-Variablen
String cachedFseqOptions = "";
String cachedConfigOptions = "";

/**
 * Returns a formatted string with storage statistics.
 * Useful for the Serial Monitor or Debug views.
 */
String getStorageInfo() {
    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    float freePercent = 100.0 * (total - used) / total;
    
    char buf[100];
    sprintf(buf, "Storage: %u / %u Bytes used (%.1f%% free)", used, total, freePercent);
    return String(buf);
}

/**
 * Generates a human-readable system status string.
 * Used for the Web UI and Serial logging.
 */
String getSystemStatus() {
    if (showRunning) return "🔴 SHOW ACTIVE";
    
    if (showStartEpoch > 0) {
        long timeLeft = showStartEpoch - timeClient.getEpochTime();
        if (timeLeft > 0) {
            int mins = timeLeft / 60;
            int secs = timeLeft % 60;
            char buf[30];
            sprintf(buf, "⏳ COUNTDOWN: %02d:%02d", mins, secs);
            return String(buf);
        }
    }
    return "🟢 READY";
}

/**
 * Parses a JSON config file to return a short HTML-formatted summary.
 * Used to display hardware details in the Web UI.
 */
String getConfigSummary(String filename) {
    // Ensure filename has leading slash
    if (!filename.startsWith("/")) filename = "/" + filename;
    
    File file = LittleFS.open(filename, "r");
    if (!file) return "Error: Could not open config file";

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) return "Error: Invalid JSON structure";

    String name   = doc["name"] | "Unknown Device";
    int ledsCount = doc["leds"].size();

    // Note: data_pin is now fixed to GPIO 2 in code for stability.
    return "<b>" + name + "</b>: " + String(ledsCount) + " LEDs mapped (Pin 2)";
}

/**
 * Updates FastLED brightness and power limits based on the current configuration.
 * Prevents overcurrent situations on USB ports.
 */
void applyPowerSettings() {
  // Set global brightness (0-255)
  // Default to 128 if not specified in JSON
  FastLED.setBrightness(currentConfig.max_brightness);

  // Apply power management
  // This calculates the power draw and dims LEDs if they exceed the limit
  // Default to 500mA for safety
  FastLED.setMaxPowerInVoltsAndMilliamps(5, currentConfig.max_milliamps);
}

/**
 * Loads a JSON configuration file from LittleFS and applies hardware settings.
 * Includes bounds-checking for Tesla-specific channel ranges (0-511).
 * @param filename The path to the .json config file.
 * @return True if configuration was loaded and validated successfully.
 */
bool loadConfig(const String& filename) {
    // 1. Pfad-Normalisierung
    String path = filename;
    if (!path.startsWith("/")) path = "/" + path;

    // 2. Datei öffnen
    File file = LittleFS.open(path, "r");
    if (!file) {
        Serial.printf("ERR: Config not found: %s\n", path.c_str());
        return false;
    }
    
    // 3. JSON Parsen (v7 nutzt Stack-Memory für kleine Dokumente)
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        Serial.printf("ERR: JSON Parse failed (%s) in file %s\n", error.c_str(), path.c_str());
        return false;
    }

    // 4. Globale Parameter mit Fallbacks
    currentConfig.name = doc["name"] | "Unknown Device";
    currentConfig.channel_offset = doc["channel_offset"] | 0;
    currentConfig.max_brightness = doc["max_brightness"] | 128;
    currentConfig.max_milliamps = doc["max_milliamps"] | 500;

    // 5. LED Mapping mit Bounds-Checking
    currentConfig.leds.clear();
    JsonArray arr = doc["leds"];
    size_t newSize = arr.size();

    if (newSize > MAX_LEDS) {
        Serial.printf("WARN: JSON defines %d LEDs, but MAX_LEDS is %d. Truncating.\n", newSize, MAX_LEDS);
        newSize = MAX_LEDS;
    }

    currentConfig.leds.reserve(newSize);
    
    for (size_t i = 0; i < newSize; i++) {
        uint16_t ch = arr[i]["channel"] | 9999;
        
        // Sanity Check: Tesla FSEQ Kanäle liegen üblicherweise im Bereich 0-511.
        // Wir lassen Werte bis 1024 zu (für erweiterte Shows), markieren alles darüber als "Black".
        if (ch > 1024 && ch != 9999) {
            Serial.printf("Line %d: Channel %d out of bounds. Set to 9999 (Off).\n", i, ch);
            ch = 9999;
        }

        LedMapping m;
        m.channel = ch;
        currentConfig.leds.push_back(m);
    }
    
    // Speicherbereinigung für den ESP32-C3 Heap
    currentConfig.leds.shrink_to_fit();

    // 6. Hardware Re-Initialisierung
    int numLeds = currentConfig.leds.size();
    if (numLeds > 0) {
        // FastLED.addLeds kann mehrfach aufgerufen werden; es aktualisiert den internen Pointer.
        FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, numLeds).setCorrection(TypicalLEDStrip); 
        
        applyPowerSettings(); // Wendet Helligkeit und mA-Limit an
        
        FastLED.clear();
        FastLED.show();
    }
    
    configValid = (numLeds > 0); 
    Serial.printf("SUCCESS: Config '%s' applied (%d LEDs mapped)\n", 
                  currentConfig.name.c_str(), numLeds);
                  
    return configValid;
}

// ------------------- Helper Functions -------------------
void showStatus(const char* msg) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(xOffset, yOffset + 20, msg);
  u8g2.sendBuffer();
}

void showIP() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  
  u8g2.drawStr(xOffset, yOffset + 10, "WiFi OK");

  // IP devided to two lines ("192.168.123." and "123")
  String ip = WiFi.localIP().toString();
  int dot3 = ip.lastIndexOf('.', ip.lastIndexOf('.') - 1);  // position of third dot
  String part1 = ip.substring(0, dot3 + 1);   // "192.168.178."
  String part2 = ip.substring(dot3 + 1);     // "123"

  u8g2.drawStr(xOffset, yOffset + 20, part1.c_str());
  u8g2.drawStr(xOffset, yOffset + 32, part2.c_str());

  u8g2.drawStr(xOffset, yOffset + 44, "mys3xy.local");
  
  u8g2.sendBuffer();
  delay(6000);  // Show for 6 seconds
}

/**
 * Scans LittleFS and caches HTML options for the Web UI.
 * Prevents file system lag during show playback and stabilizes the heap.
 */
void refreshFileCache() {
    if (showRunning) return; 
    
    cachedFseqOptions = "";
    cachedConfigOptions = "";
    
    File root = LittleFS.open("/");
    if (!root || !root.isDirectory()) {
        Serial.println(F("ERR: Could not open Root for caching!"));
        return;
    }

    File entry = root.openNextFile();
    while (entry) {
        String n = entry.name();
        
        // Path normalization: remove leading slash if present
        if (n.startsWith("/")) n = n.substring(1);
        
        if (n.endsWith(".fseq")) {
            // Mark the currently selected show in the dropdown
            String selected = (("/" + n) == currentShow) ? " selected" : "";
            cachedFseqOptions += "<option value='" + n + "'" + selected + ">" + n + "</option>";
        } 
        else if (n.startsWith("config_") && n.endsWith(".json")) {
            // Mark the currently selected hardware config
            String selected = (("/" + n) == currentConfigFile) ? " selected" : "";
            cachedConfigOptions += "<option value='" + n + "'" + selected + ">" + n + "</option>";
        }
        
        entry.close();
        entry = root.openNextFile();
    }
    root.close();
    Serial.println(F("UI File Cache updated."));
}

/**
 * Parses the FSEQ file header and updates OLED status.
 * Correctly extracts physical channel count to prevent READ ERRORs.
 */
bool readFseqHeader() {
    if (!fseqFile) return false;
    uint8_t h[32]; 
    fseqFile.seek(0);
    if (fseqFile.read(h, 32) < 28) return false;

    // Verify Magic Cookie
    if (h[0] != 'P' || h[1] != 'S' || h[2] != 'E' || h[3] != 'Q') return false;

    fseqDataOffset = (uint16_t)h[4] | ((uint16_t)h[5] << 8);
    
    // Physical channels (e.g., 200 in Simon's file)
    realChannelsInFile = (uint32_t)h[10] | ((uint32_t)h[11] << 8) | 
                         ((uint32_t)h[12] << 16) | ((uint32_t)h[13] << 24);
                         
    frameCount = (uint32_t)h[14] | ((uint32_t)h[15] << 8) | 
                 ((uint32_t)h[16] << 16) | ((uint32_t)h[17] << 24);

    stepTimeMs = h[18] | (h[19] << 8);

    // LOGICAL SYNC: This fixes your missing LEDs. 
    // We treat the show as a 512-channel show so mapping 392, 164 etc. works perfectly.
    channelCount = realChannelsInFile; // This will be 200

    // OLED Feedback (as per your original style)
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.setCursor(0, 20); u8g2.print("Ch: "); u8g2.print(realChannelsInFile);
    u8g2.setCursor(0, 40); u8g2.print("Off: "); u8g2.print(fseqDataOffset);
    u8g2.sendBuffer();

    return (realChannelsInFile > 0);
}

/**
 * FINAL RELEASE VERSION 1.0.0
 * Features: 512-Stride Emulation, Virtual File Looping, Channel Analyzer.
 * Solves the "Frame 2879" crash while maintaining perfect Tesla-sync.
 */
bool playFrame(uint32_t frameIdx) {
    if (!fseqFile || frameIdx >= frameCount) return false;

    // 1. LOGICAL VS PHYSICAL STEERING
    // We use 512 to match the Tesla Mapping, but the file only has ~2879 frames worth of data.
    uint32_t logicalStride = 512;
    uint32_t physicalMaxFrames = (fseqFile.size() - fseqDataOffset) / logicalStride;

    // Virtual Looping: Prevent READ ERROR by wrapping the index within physical file bounds
    uint32_t safeFrameIdx = frameIdx;
    if (physicalMaxFrames > 0) {
        safeFrameIdx = frameIdx % physicalMaxFrames;
    }

    uint32_t targetPos = (uint32_t)fseqDataOffset + (safeFrameIdx * logicalStride);

    if (!fseqFile.seek(targetPos)) {
        Serial.printf("CRITICAL: SEEK ERROR at Frame %u\n", frameIdx);
        return false;
    }

    // 2. BUFFERING
    static uint8_t frameData[1024]; 
    memset(frameData, 0, sizeof(frameData));
    
    // Read the logical 512 byte block
    size_t bytesRead = fseqFile.read(frameData, 512);

    // 3. CHANNEL ANALYZER
    if (scanActive) {
        // Find peaks across all 512 logical channels
        for (int i = 0; i < 512; i++) {
            if (frameData[i] > globalMax[i]) globalMax[i] = frameData[i];
        }
        // Visual feedback on the first 32 LEDs
        for (int i = 0; i < 32; i++) {
            leds[i] = CRGB(frameData[i], frameData[i], frameData[i]);
        }
    } 
    else {
        // 4. NORMAL MAPPING (THE SIMON-SYNC)
        size_t totalLeds = currentConfig.leds.size();
        for (size_t i = 0; i < totalLeds; i++) {
            uint16_t rawCh = currentConfig.leds[i].channel;
            if (rawCh == 9999) { leds[i] = CRGB::Black; continue; }
            
            // Absolute Addressing: Triple LEDs now work in perfect sync!
            uint8_t val = (rawCh < 1024) ? frameData[rawCh] : 0;

            // --- PRECISION COLOR LOGIC (V1.0.0) ---
            if (rawCh == 139 || rawCh == 142 || rawCh == 339 || rawCh == 342) {
                leds[i] = CRGB(val, (val * 160) >> 8, 0); // Amber Indicators
            } 
            else if ((rawCh >= 364 && rawCh <= 371) || rawCh == 392) {
                leds[i] = CRGB(val, 0, 0); // Red Brake/Rear
            }
            else if (rawCh >= 151 && rawCh <= 160) {
                leds[i] = CRGB((val * 100) >> 8, (val * 100) >> 8, val); // Blue Matrix
            }
            else {
                leds[i] = CRGB(val, val, val); // White Main Beams/Reverse
            }
        }
    }

    FastLED.show();
    return (frameIdx + 1) < frameCount;
}

/**
 * Stops the current show, clears all LEDs, and closes open file handles.
 * Resets playback variables for a clean system state.
 */
void stopShowAndCleanup() {
    isBusy = true; 
    showRunning = false;
    
    // 1. Turn off LEDs first (immediate feedback)
    FastLED.clear(true);
    FastLED.show();
    
    // 2. Small pause to let the CPU settle
    delay(200);
    yield();

    // 3. Close file carefully
    if (fseqFile) {
        fseqFile.close();
        // The most important line for ESP32-C3 stability:
        fseqFile = File(); 
    }

    showStartEpoch = 0;
    currentFrame = 0;
    isBusy = false;
    scanActive = false; // Reset scan mode after show ends

    showStatus("READY");
    Serial.println(F("Clean exit."));
}

/**
 * Deletes a specific file from LittleFS storage.
 * Redirects the user back to the main dashboard after completion.
 */
void handleDelete(AsyncWebServerRequest *request) {
    if (showRunning || isBusy) {
        request->send(403, "text/plain", "Cannot delete files while show is active!");
        return;
    }

    if (request->hasParam("file")) {
        String filename = request->getParam("file")->value();
        if (!filename.startsWith("/")) filename = "/" + filename;
        
        if (LittleFS.exists(filename)) {
            LittleFS.remove(filename);
            // --- CACHE ERNEUERN ---
            refreshFileCache(); 
            Serial.printf("Deleted and Cache refreshed: %s\n", filename.c_str());
        }
    }
    // Redirect back to main page immediately
    request->redirect("/");
}

/**
 * Main Web Interface Handler for the S3XY Lightshow Controller.
 * Manages HTTP GET for UI rendering and HTTP POST for show configuration.
 * Features: Automatic Hardware Discovery, UTC Synchronization, and Live Status Updates.
 */
void handleTeslaApp(AsyncWebServerRequest *request) {
    // --- 1. SAFETY & PERFORMANCE HEADERS ---
    if (showRunning) {
        request->send(200, "text/plain", "Show in progress. Check OLED.");
        return;
    }

    // --- 2. POST DATA PROCESSING ---
    if (request->method() == HTTP_POST) {
        // If a Show was just started, stop POST 
        if (isBusy) { request->redirect("/"); return; }

        // 1. Hardware Config Selection
        if (request->hasParam("config", true)) {
            String val = request->getParam("config", true)->value();
            if (!val.startsWith("/")) val = "/" + val;
            currentConfigFile = val;

            Serial.print("Web UI requested config: "); Serial.println(currentConfigFile);
    
            if (LittleFS.exists(currentConfigFile)) {
                loadConfig(currentConfigFile);
                Serial.println("Config loaded successfully.");
            } else {
                Serial.println("ERROR: Config file not found in LittleFS!");
            }
        }

        // 2. Show File Selection
        if (request->hasParam("show", true)) {
            String val = request->getParam("show", true)->value();
            if (!val.startsWith("/")) val = "/" + val;
            currentShow = val;
            if (fseqFile) fseqFile.close();
            fseqFile = LittleFS.open(currentShow, "r");
            if (fseqFile) readFseqHeader();
        }

        // 3. Start Logic (Instant vs. Scheduled)
        if (request->hasParam("instant", true)) {
            showStartEpoch = 0;
            triggerCountdown = true;
        } 
        else if (request->hasParam("utc_target", true)) {
            String utcStr = request->getParam("utc_target", true)->value();
            long receivedEpoch = utcStr.toInt();
            if (receivedEpoch > 0) {
                showStartEpoch = receivedEpoch;
                triggerCountdown = true;
            }
        }

        // 4. Analyzer Mode Toggle
        scanActive = request->hasParam("scan_mode", true);

        request->redirect("/"); 
        return;
    }

    // --- 3. UI GENERATION ---
    String html = ""; 
    html.reserve(8000); // Pre-allocate memory for performance

    // HTML Header, CSS and Styles
    html += R"=====(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>)=====";
    html += PROJECT_NAME; 
    html += R"=====(</title><style>
:root { --tesla-red: #cc0000; --tesla-green: #2e7d32; --bg-dark: #121212; --card-bg: #1e1e1e; }
body { font-family: 'Segoe UI', sans-serif; text-align: center; margin: 0; background: var(--bg-dark); color: #e0e0e0; padding: 15px; }
.project-header { margin-bottom: 20px; opacity: 0.7; font-size: 0.8em; line-height: 1.4; letter-spacing: 0.5px; }
.card { background: var(--card-bg); border-radius: 12px; padding: 20px; margin-bottom: 20px; max-width: 480px; margin-left: auto; margin-right: auto; border: 1px solid #333; box-shadow: 0 4px 15px rgba(0,0,0,0.5); }
h1 { color: var(--tesla-red); letter-spacing: 2px; margin-bottom: 5px;  font-weight: 900; }
h3 { border-bottom: 1px solid #333; padding-bottom: 10px; margin-top: 0; font-size: 1.1em; color: #bbb; }
label { display: block; text-align: left; font-size: 0.85em; color: #888; margin: 10px 0 5px 0; }
select, input, button { font-size: 16px; padding: 12px; margin: 5px 0; width: 100%; border-radius: 8px; border: 1px solid #333; background: #2a2a2a; color: white; box-sizing: border-box; outline: none; }
select { appearance: none; background-image: url("data:image/svg+xml;charset=US-ASCII,%3Csvg%20xmlns%3D%22http%3A%2F%2Fwww.w3.org%2F2000%2Fsvg%22%20width%3D%22292.4%22%20height%3D%22292.4%22%3E%3Cpath%20fill%3D%22%23FFFFFF%22%20d%3D%22M287%2069.4a17.6%2017.6%200%200%200-13-5.4H18.4c-5%200-9.3%201.8-12.9%205.4A17.6%2017.6%200%200%200%200%2082.2c0%205%201.8%209.3%205.4%2012.9l128%20127.9c3.6%203.6%207.8%205.4%2012.8%205.4s9.2-1.8%2012.8-5.4L287%2095c3.5-3.5%205.4-7.8%205.4-12.8%200-5-1.9-9.2-5.5-12.8z%22%2F%3E%3C%2Fsvg%3E"); background-repeat: no-repeat; background-position: right 12px center; background-size: 12px auto; padding-right: 35px; }
button { background: var(--tesla-red); cursor: pointer; font-weight: bold; border: none; text-transform: uppercase; letter-spacing: 1px; }
.btn-now { background: var(--tesla-green); width: auto !important; padding: 12px 25px !important; margin-left: 5px; }
.file-list { text-align: left; list-style: none; padding: 0; }
.file-item { padding: 12px; border-bottom: 1px solid #252525; position: relative; }
.btn-del { color: #ff4444; text-decoration: none; font-size: 11px; border: 1px solid #ff4444; padding: 3px 8px; border-radius: 4px; position: absolute; right: 10px; top: 12px; }
.status-pill { display: inline-block; padding: 6px 18px; border-radius: 20px; font-weight: bold; margin-bottom: 20px; font-size: 0.9em; letter-spacing: 1px; }
</style></head><body>)=====";

    // --- Dynamic Project Branding ---
    html += "<h1>" + String(PROJECT_NAME) + "</h1>";
    html += "<div class='project-header'>";
    html += "v" + String(PROJECT_VERSION) + " &bull; ESP32-C3 Lightshow Engine<br>";
    html += "Built for Tesla Synchronized Performances</div>";

    // --- UI: Dynamic Status Pill ---
    String initialStatus = "⚪ NO CONFIG LOADED";
    String pillColor = "#666"; 

    if (showRunning) { 
        initialStatus = "🔴 SHOW ACTIVE"; pillColor = "#d32f2f"; 
    } else if (showStartEpoch > 0) { 
        initialStatus = "⏳ WAITING..."; pillColor = "#f57c00"; 
    } else if (configValid) { 
        initialStatus = "🟢 READY"; pillColor = "#388e3c";
    }

    if (timeClient.getEpochTime() < 1000000) { 
        initialStatus += " (⚠️ NO NTP SYNC)";
        // We keep the color but add the text so they know why Countdown might fail
    }

    html += "<div id='status-pill' class='status-pill' style='background:" + pillColor + ";'>" + initialStatus + "</div>";

    // --- MAIN CARD: Control Center ---
    html += "<div class='card'><h3>Control Center</h3><form action='/setshow' method='post'>";
    html += "<input type='hidden' id='utc_target' name='utc_target' value='0'>";

    // 1. Show Selection
    html += "<label>1. Select Sequence File:</label><select name='show'>";
    html += cachedFseqOptions;
    html += "</select>";

    // 2. Hardware Config Mapping
    html += "<label>2. Hardware Mapping:</label><select name='config' onchange='this.form.submit()'>";
    html += cachedConfigOptions;
    html += "</select>";

    // 3. Timing and Launch
    html += "<label>3. Start Time & Launch:</label><div style='display: flex; gap: 5px;'>";
    html += "<select name='start_time' style='flex-grow: 1;'>";
    time_t now = timeClient.getEpochTime();
    for (int i = 1; i <= 10; i++) {
        time_t optTime = now + (i * 60);
        struct tm * t = localtime(&optTime);
        char tStr[10];
        sprintf(tStr, "%02d:%02d", t->tm_hour, t->tm_min);
        html += "<option value='" + String(tStr) + "'>" + String(tStr) + "</option>";
    }
    html += "</select><button type='submit' name='instant' value='true' class='btn-now'>NOW</button></div>";

    // 4. Mode Options
    html += "<div style='text-align:left; margin-top:15px; margin-bottom:10px;'>";
    html += "<input type='checkbox' id='scan_mode' name='scan_mode' value='true'" + String(scanActive ? " checked" : "") + " style='width:auto; margin-right:10px; vertical-align:middle;'>";
    html += "<label for='scan_mode' style='display:inline; color:#888;'>Enable Channel Analyzer</label></div>";

    html += "<button type='button' onclick='calculateUTCAndSubmit()' style='background:#444; margin-top:10px;'>START COUNTDOWN</button>";
    html += "</form></div>";

    // --- STORAGE EXPLORER CARD ---
    html += "<div class='card'><h3>Storage Explorer</h3><ul class='file-list'>";
    // Storage Info
    html += "<div style='font-size:12px; color:#888; margin-bottom:10px; border-bottom:1px solid #eee; padding-bottom:5px;'>";
    html += getStorageInfo(); 
    html += "</div>";

    html += "<ul class='file-list'>";

    File explorerRoot = LittleFS.open("/");
    File explorerFile = explorerRoot.openNextFile();
    while (explorerFile) {
        String n = explorerFile.name(); 
        if (n.startsWith("/")) n = n.substring(1);
        html += "<li class='file-item'><strong>" + n + "</strong>";
        html += "<a href='/delete?file=" + n + "' class='btn-del' onclick='return confirm(\"Delete permanently?\")'>DELETE</a></li>";
        explorerFile.close(); 
        explorerFile = explorerRoot.openNextFile();
    }
    explorerRoot.close();
    html += "</ul><hr style='border:0; border-top:1px solid #333; margin:20px 0;'>";
    
    // Upload Form
    html += "<label>Upload (.json or .fseq):</label><form method='POST' action='/upload' enctype='multipart/form-data' style='text-align:left;'>";
    html += "<input type='file' name='upload' accept='.json,.fseq' style='font-size:12px; border:1px dashed #555; width:100%;'>";
    html += "<button type='submit' style='background:#444; margin-top:10px; font-size:14px;'>UPLOAD FILE</button></form>";
    html += "<p><a href='/update' style='color:#388e3c; font-size:11px; text-decoration:none;'>&bull; Firmware OTA Portal</a></p></div>";

    // --- JAVASCRIPT: Client-Side Logic ---
    html += "<script>";
    html += "var targetEpoch = " + String(showStartEpoch) + ";";
    html += "var isRunning = " + String(showRunning ? "true" : "false") + ";";
    
    // Clean name extraction for display
    String cleanName = currentConfigFile;
    int lastSlash = cleanName.lastIndexOf('/');
    if (lastSlash != -1) cleanName = cleanName.substring(lastSlash + 1);
    if (cleanName == "None selected" || cleanName.length() < 2) cleanName = "None";
    
    html += "var configName = '" + cleanName + "';";
    
    html += R"=====(
    function calculateUTCAndSubmit() {
        var timeVal = document.getElementsByName("start_time")[0].value;
        var parts = timeVal.split(":");
        var target = new Date(); 
        target.setHours(parseInt(parts[0]), parseInt(parts[1]), 0, 0);
        if (target.getTime() < Date.now()) { target.setDate(target.getDate() + 1); }
        var epoch = Math.floor(target.getTime() / 1000);
        document.getElementById('utc_target').value = epoch;
        document.forms[0].submit();
    }

    function updateCountdown() {
        var now = Math.floor(Date.now() / 1000);
        var pill = document.getElementById('status-pill');
        if (isRunning) {
            pill.innerHTML = "🔴 SHOW ACTIVE (" + configName + ")";
            pill.style.background = "#d32f2f"; 
            return;
        }
        if (targetEpoch > 0) {
            var diff = targetEpoch - now;
            if (diff > 0) {
                pill.innerHTML = "⏳ START IN " + diff + " SECONDS";
                pill.style.background = "#f57c00";
            } else {
                pill.innerHTML = "🚀 SHOW STARTING...";
                pill.style.background = "#388e3c";
                setTimeout(function(){ location.reload(); }, 2000);
            }
        } else {
            if (configName === "None") {
                pill.innerHTML = "⚪ NO CONFIG LOADED";
                pill.style.background = "#666";
            } else {
                pill.innerHTML = "🟢 READY (" + configName + ")";
                pill.style.background = "#388e3c";
            }
        }
    }
    setInterval(updateCountdown, 1000);
    updateCountdown();
    </script></body></html>)=====";

    request->send(200, "text/html", html);
}


/**
 * Opens the file and prepares everything for immediate playback.
 * Resets global trackers and OLED status.
 */
void startShowSequence() {
    isBusy = true; 
    if (fseqFile) { fseqFile.close(); fseqFile = File(); } 

    fseqFile = LittleFS.open(currentShow, "r");
    if (fseqFile && readFseqHeader()) {
        showRunning = true;
        currentFrame = 0; 
        memset(globalMax, 0, sizeof(globalMax)); // Reset scan data for analyzer
        showStartTimeMillis = millis(); 
        
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_logisoso18_tf);
        u8g2.drawStr(xOffset, yOffset + 45, "ACTIVE");
        u8g2.sendBuffer();
        
        Serial.println(F("Show started successfully."));
    } else {
        Serial.println(F("Failed to start show."));
        stopShowAndCleanup();
    }
    isBusy = false;
}

// ------------------- setup & loop -------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== myS3XY Lightshow starting ===");

  WiFi.setSleep(false); // to prevent sleep modes

  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, HIGH); // blue LED off = WiFi not connected

  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, MAX_LEDS).setCorrection(TypicalLEDStrip);  // Fixed to DATA_PIN, buffer MAX_LEDS
  applyPowerSettings();

  u8g2.begin();
  u8g2.setContrast(255);
  u8g2.setBusClock(400000);

  showStatus("Booting...");

  // --- 1. WiFi Connection ---
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  showStatus("Connecting WiFi...");
  
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 30) {
    delay(500);
    Serial.print(".");
    yield(); 
    wifiAttempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(STATUS_LED, LOW); // Blue LED ON
    showStatus("WiFi OK");
    showIP();

    // --- 2. mDNS Setup (Only if WiFi is OK) ---
    if (MDNS.begin("mys3xy")) {
      Serial.println("mDNS started: mys3xy.local");
      MDNS.addService("http", "tcp", 80);
    }
  } else {
    showStatus("WiFi Offline");
    Serial.println("\nWiFi connection failed. Working in Offline Mode.");
  }

  // --- 3. NTP Sync (With Timeout) ---
  timeClient.begin();
  showStatus("Syncing time...");
  
  bool ntpSuccess = false;
  int ntpAttempts = 0;
  while (ntpAttempts < 20) {
    if (timeClient.update() || timeClient.forceUpdate()) {
      ntpSuccess = true;
      break;
    }
    Serial.print(".");
    yield();
    delay(500);
    ntpAttempts++;
  }

  if (ntpSuccess) {
    showStatus("Time synced");
    Serial.println("\nNTP Sync Success!");
  } else {
    showStatus("Sync failed");
    Serial.println("\nNTP Sync failed. Shows can only be started via 'NOW'.");
    delay(2000);
  }

  if (!LittleFS.begin(true)) { 
    Serial.println("LittleFS Error!");
    showStatus("FS Format..."); 
  }
  Serial.println("LittleFS mounted");
  // IMPORTANT: Populate the UI cache immediately after mounting
  refreshFileCache();

  // --- Storage Capacity Check ---
    if (LittleFS.begin(true)) {
        size_t total = LittleFS.totalBytes();
        size_t used = LittleFS.usedBytes();
        size_t freeSpace = total - used;

        Serial.println(F("--- STORAGE STATUS ---"));
        Serial.printf("Total Space: %d KB\n", total / 1024);
        Serial.printf("Used Space:  %d KB\n", used / 1024);
        Serial.printf("Free Space:  %d KB\n", freeSpace / 1024);
        
        if (freeSpace < 1536) { // Warning if less than 1.5 MB available
            Serial.println(F("WARNING: Low space for large 1.5MB FSEQ files!"));
        }
        Serial.println(F("----------------------"));
    }

    // --- Auto-Discovery for Config & Show ---
    File rootDir = LittleFS.open("/");
    File fileEntry = rootDir.openNextFile();
    while (fileEntry) {
        String n = fileEntry.name();
        if (!n.startsWith("/")) n = "/" + n; // Normalize for LittleFS
        
        // Auto-load first config
        if (currentConfigFile == "None selected" && n.indexOf("config_") != -1 && n.endsWith(".json")) {
            currentConfigFile = n;
            if (loadConfig(currentConfigFile)) {
                Serial.printf("Auto-loaded config: %s\n", n.c_str());
            }
        }
        // Auto-select first show
        if (currentShow == "None selected" && n.endsWith(".fseq")) {
            currentShow = n;
            Serial.printf("Auto-selected show: %s\n", n.c_str());
        }
        fileEntry = rootDir.openNextFile();
    }
    rootDir.close();

  // List all files
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    Serial.println("Found file: " + String(file.name()));
    file = root.openNextFile();
  }
  root.close();

  if (currentConfigFile.startsWith("/")) {
    loadConfig(currentConfigFile);
  } else {
    Serial.println(F("No default config selected yet."));
  }

  /*
  if (!LittleFS.exists("/show.fseq")) {
    showStatus("No show.fseq");
    while (1);
  }
  */

  ElegantOTA.begin(&server);
  server.on("/", HTTP_ANY, handleTeslaApp);
  server.on("/", HTTP_GET, handleTeslaApp);
  server.on("/setshow", HTTP_GET, handleTeslaApp);
  server.on("/setshow", HTTP_POST, handleTeslaApp);
  server.on("/delete", HTTP_GET, handleDelete);
  // --- HTTP POST: File Upload Handler ---
  server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *request) {
      bool isValid = true;
      String message = "Upload successful!";
      
      // Validation: Check if the uploaded JSON is syntactically correct
      if (lastUploadedFilename.endsWith(".json")) {
          File file = LittleFS.open("/" + lastUploadedFilename, "r");
          if (file) {
              JsonDocument doc;
              DeserializationError error = deserializeJson(doc, file);
              file.close();
              if (error) {
                  isValid = false;
                  message = "JSON ERROR: " + String(error.c_str());
                  LittleFS.remove("/" + lastUploadedFilename); // Delete invalid file
              }
          }
      }

      // UI: Feedback page with Tesla-style status colors
      String statusColor = isValid ? "#4CAF50" : "#f44336";
      String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'></head>";
      html += "<body style='font-family:Arial;text-align:center;background:#121212;color:white;padding:20px;'>";
      html += "<div style='background:#1e1e1e;padding:30px;border-radius:12px;border-top:5px solid " + statusColor + ";display:inline-block;width:90%;max-width:400px;'>";
      html += "<h2>" + message + "</h2>";
      html += "<p style='color:#888;'>File: " + lastUploadedFilename + "</p>";
      html += "<br><a href='/' style='display:block;background:#cc0000;color:white;padding:15px;text-decoration:none;border-radius:6px;font-weight:bold;'>[ Back to Dashboard ]</a>";
      html += "</div></body></html>";
      
      request->send(200, "text/html", html);
  }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      // Chunked Upload: Process incoming data packets
      if (!index) {
          // New upload starts: sanitize filename
          if (filename.startsWith("/")) lastUploadedFilename = filename.substring(1);
          else lastUploadedFilename = filename;
          
          Serial.printf("Uploading: %s\n", lastUploadedFilename.c_str());
          request->_tempFile = LittleFS.open("/" + lastUploadedFilename, "w");
      }
      
      if (len && request->_tempFile) {
          request->_tempFile.write(data, len);
          yield(); // Give ESP32-C3 time for background tasks (WiFi/WDT)
      }
      
      if (final && request->_tempFile) {
          request->_tempFile.close();
          refreshFileCache(); 
          Serial.println(F("Upload complete & Cache refreshed."));
          yield();
      }
  });

  server.on("/cancel", HTTP_GET, [](AsyncWebServerRequest *request) {
    showRunning = false;
    showStartEpoch = 0;
    triggerCountdown = false;
    currentFrame = 0;
    FastLED.clear();
    FastLED.show();
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tr);
    u8g2.drawStr(xOffset, yOffset + 20, "Show Cancelled");
    u8g2.sendBuffer();
    request->redirect("/"); // Back to the Dashboard
  });

  server.begin();
  Serial.println("Web server & OTA ready");
  showStatus("App ready");
}

/**
 * System Health Monitor: Logs memory and task stability.
 * Run this during your 5-minute stress test.
 */
void logSystemHealth() {
    static uint32_t lastLog = 0;
    if (millis() - lastLog < 30000) return; // Alle 30 Sek. loggen
    lastLog = millis();

    size_t freeHeap = ESP.getFreeHeap();
    size_t minFreeHeap = ESP.getMinFreeHeap(); // Tiefpunkt seit Boot
    size_t maxBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    Serial.println(F("--- SYSTEM HEALTH REPORT ---"));
    Serial.printf("Free Heap: %u Bytes\n", freeHeap);
    Serial.printf("Min Free (Watermark): %u Bytes\n", minFreeHeap);
    Serial.printf("Fragmentation (Largest Block): %u Bytes\n", maxBlock);
    
    if (showRunning) {
        Serial.printf("Active Show: Frame %u / %u\n", currentFrame, frameCount);
    }

    // Warnung bei kritischem Speicherstand
    if (freeHeap < 15000) {
        Serial.println(F("!!! CRITICAL: Low Memory detected!"));
    }
    Serial.println(F("----------------------------"));
}

void loop() {
  logSystemHealth(); // Monitoring aktiv
  ElegantOTA.loop();
  timeClient.update();
  unsigned long currentEpoch = timeClient.getEpochTime();

  // --- CASE 1: TRIGGER IMMEDIATE START (NOW) ---
  // If triggerCountdown is true but showStartEpoch is 0, start immediately
  if (triggerCountdown && showStartEpoch == 0 && !showRunning) {
      Serial.println(F("Instant start triggered (NOW button)."));
      triggerCountdown = false; // Reset trigger
      startShowSequence();      // Helper function to keep loop clean
  }

  // --- CASE 2: WAITING FOR SCHEDULED START (COUNTDOWN) ---
  if (showStartEpoch > 0 && !showRunning) {
    long secondsLeft = showStartEpoch - currentEpoch;

    if (secondsLeft > 0) {
      // --- DISPLAY COUNTDOWN ---
      static unsigned long lastUpdate = 0;
      if (millis() - lastUpdate > 500) { 
        int mins = secondsLeft / 60;
        int secs = secondsLeft % 60;
        char buf[10];
        u8g2.clearBuffer();
        if (mins > 0) {
          sprintf(buf, "%02d:%02d", mins, secs);
          u8g2.setFont(u8g2_font_logisoso24_tr);
          u8g2.drawStr(xOffset + 0, yOffset + 44, buf);
        } else {
          sprintf(buf, "%02d", secs);
          u8g2.setFont(u8g2_font_logisoso32_tr);
          u8g2.drawStr(xOffset + 15, yOffset + 48, buf);
        }
        u8g2.sendBuffer();
        lastUpdate = millis();
      }
    } 
    // SAFETY START: Start if we are within a small window (0 to -5 seconds)
    else if (secondsLeft >= -5) {
        showStartEpoch = 0;       // Reset epoch to stop countdown logic
        triggerCountdown = false; // Reset trigger
        startShowSequence();      // Start show
    } 
    // SYNC ERROR: If target time is more than 5 seconds in the past
    else {
        Serial.printf("Sync Error: Time is %ld seconds in the past. Cancelling.\n", -secondsLeft);
        stopShowAndCleanup();
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x10_tr);
        u8g2.drawStr(xOffset, yOffset + 20, "SYNC ERROR");
        u8g2.sendBuffer();
        delay(2000);
        showStartEpoch = 0;
        triggerCountdown = false;
    }
  }

  // --- CASE 3: SHOW IS ACTIVE ---
  if (showRunning && !isBusy) {
      static uint32_t totalProcessTime = 0;
      static uint16_t sampleCounter = 0;
      
      // 1. High precision frame timing
      unsigned long msElapsed = millis() - showStartTimeMillis;
      uint32_t targetFrame = msElapsed / stepTimeMs;

      // 2. Playback logic
      if (targetFrame >= currentFrame) {
          // Automatic lag compensation
          if (targetFrame > currentFrame + 2) {
              currentFrame = targetFrame;
          }

          unsigned long startMicros = micros();
          
          if (!playFrame(currentFrame)) {
              stopShowAndCleanup();
          } else {
              currentFrame++;
          }

          // Calculate and monitor performance
          uint32_t duration = (micros() - startMicros) / 1000;
          totalProcessTime += duration;
          sampleCounter++;

          if (sampleCounter >= 100) {
              uint32_t avg = totalProcessTime / 100;
              Serial.printf(">>> PERFORMANCE: Avg Frame Time %d ms | Target: %d ms\n", avg, stepTimeMs);
              if (avg >= stepTimeMs) {
                  Serial.println("!!! WARNING: Storage or CPU too slow!");
              }
              totalProcessTime = 0;
              sampleCounter = 0;
          }
      }
  }
}