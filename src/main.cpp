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

// ------------------- Configuration -------------------
const char* ssid     = "LIGHTSHOW";
const char* password = "mys3xyls";

#define STATUS_LED 8      // Onboard LED for WiFi status
#define DATA_PIN   10     // Default data pin for WS2812B (can be overridden by config)

#define SCHEDULED_START true
#define MINUTES_AHEAD 5   // Default scheduled minutes ahead

#define FRAME_BUFFER_SIZE 512 
uint8_t frameData[FRAME_BUFFER_SIZE];

#define BUFFER_SIZE 15        // Number of frames to cache
uint8_t frameBuffer[BUFFER_SIZE][256]; // Adjust 256 to your max scanned channel count
uint32_t bufferedFrameIdx[BUFFER_SIZE];
int bufferHead = 0;           // Where we write into
int bufferTail = 0;           // Where we read from

// OLED (visible area 72x40 in 132x64 buffer)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ 6, /* data=*/ 5);

const int width = 72;
const int height = 40;
const int xOffset = 30;  // (132 - 72) / 2
const int yOffset = 12;  // (64 - 40) / 2

// -----------------------------------------------------

CRGB leds[100];  // Buffer for max 100 LEDs (adjusted dynamically by config)

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600, 60000);  // UTC+1 (CET)

AsyncWebServer server(80);

File fseqFile;
uint32_t channelCount = 0;
uint32_t frameCount = 0;
uint16_t stepTimeMs = 50;


bool showRunning = false;
bool triggerCountdown = false;  // New: Signal to start the countdown
bool isBusy = false; // To prevent overlapping operations
unsigned long showStartEpoch = 0;
unsigned long showStartTimeMillis = 0; // Stores the exact millisecond of the start
unsigned long lastOledUpdate = 0;
uint8_t maxValues[200]; // Storage for peak values of the first 200 channels
uint32_t currentFrame = 0; // Track current frame globally

// ------------------- Config Structure -------------------
struct LedMapping {
  uint16_t channel;
};

struct Config {
  String name = "Default";
  uint8_t data_pin = DATA_PIN;
  uint16_t channel_offset = 0;
  uint8_t max_brightness = 128;   // Default brightness
  uint16_t max_milliamps = 500;   // Default power limit
  std::vector<LedMapping> leds;
};

Config currentConfig;
String currentConfigFile = "/test.json";  // Default config
String currentShow = "/show.fseq";  // Default FSEQ-file
String lastUploadedFilename = ""; 

String getStorageInfo() {
    size_t total = LittleFS.totalBytes();
    size_t used = LittleFS.usedBytes();
    float freePercent = 100.0 * (total - used) / total;
    
    char buf[100];
    sprintf(buf, "Storage: %u / %u Bytes occupied (%.1f%% free)", used, total, freePercent);
    return String(buf);
}

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

String getConfigSummary(String filename) {
    File file = LittleFS.open("/" + filename, "r");
    if (!file) return "Error opening file";

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) return "Invalid JSON Structure";

    String name = doc["name"] | "Unknown";
    int ledsCount = doc["leds"].size();
    int dataPin = doc["data_pin"] | 0;

    return "<b>" + name + "</b>: " + String(ledsCount) + " LEDs (Pin " + String(dataPin) + ")";
}

void applyPowerSettings() {
  // Set global brightness (0-255)
  // Default to 128 if not specified in JSON
  FastLED.setBrightness(currentConfig.max_brightness);

  // Apply power management
  // This calculates the power draw and dims LEDs if they exceed the limit
  // Default to 500mA for safety
  FastLED.setMaxPowerInVoltsAndMilliamps(5, currentConfig.max_milliamps);
}

bool loadConfig(const String& filename) {
  File file = LittleFS.open(filename, "r");
  if (!file) return false;
  
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) return false;

  currentConfig.name = doc["name"] | "Unknown";
  currentConfig.data_pin = doc["data_pin"] | DATA_PIN;
  currentConfig.channel_offset = doc["channel_offset"] | 0;

  // Read new safety settings with sensible fallbacks
  currentConfig.max_brightness = doc["max_brightness"] | 128;
  currentConfig.max_milliamps = doc["max_milliamps"] | 500;

  currentConfig.leds.clear();
  currentConfig.leds.reserve(doc["leds"].size());
  JsonArray arr = doc["leds"];
  for (JsonObject obj : arr) {
    LedMapping m;
    m.channel = obj["channel"] | 0;
    currentConfig.leds.push_back(m);
  }

  applyPowerSettings(); 
  
  Serial.printf("Config Applied: %s, MaxBrightness: %u, PowerLimit: %umA\n", 
                currentConfig.name.c_str(), currentConfig.max_brightness, currentConfig.max_milliamps);
  Serial.println("Loaded config: " + currentConfig.name + " (" + String(currentConfig.leds.size()) + " LEDs)");
  return true;
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
  delay(6000);  // Etwas länger anzeigen
}

bool readFseqHeader() {
  if (!fseqFile) {
    showStatus("Err: File NULL");
    return false;
  }

  uint8_t header[28];
  fseqFile.seek(0);
  if (fseqFile.read(header, 28) != 28) {
    showStatus("Err: Header Read");
    return false;
  }

  // Magic Cookie Check: FSEQ Dateien starten immer mit 'P', 'S', 'E', 'Q'
  if (header[0] != 'P' || header[1] != 'S') {
    showStatus("Err: No FSEQ");
    return false;
  }

  channelCount = header[10] | (header[11] << 8) | (header[12] << 16) | (header[13] << 24);
  frameCount   = header[14] | (header[15] << 8) | (header[16] << 16) | (header[17] << 24);
  stepTimeMs   = header[18] | (header[19] << 8);
  uint8_t compression = header[20]; 

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.setCursor(xOffset, yOffset + 22);
  u8g2.print("Ch: "); u8g2.print(channelCount);
  u8g2.setCursor(xOffset, yOffset + 34);
  u8g2.print("Fr: "); u8g2.print(frameCount);
  u8g2.setCursor(xOffset, yOffset + 46);
  u8g2.print("Comp: "); u8g2.print(compression); 
  u8g2.sendBuffer();
  
  delay(3000); 

  // Wenn compression > 0 ist, ist die Datei für uns unlesbar (Zstd)
  if (compression != 0) return false;
  
  // Wenn channelCount 0 ist, ist es ein V2 Sparse Format
  if (channelCount == 0) return false;

  return true;
}

/**
 * Plays a single frame by reading the entire channel block into a RAM buffer first.
 * This prevents the ESP32-C3 from crashing due to too many file seeks.
 */
/*
bool playFrame(uint32_t frameIdx) {
    // 1. SAFETY CHECK
    if (!fseqFile || frameIdx >= frameCount) return false;

    // 2. FILE POSITIONING
    // FSEQ Header is 28 bytes. Each frame has 'channelCount' bytes.
    uint32_t frameOffsetBase = 28 + (frameIdx * channelCount);
    uint32_t startByteInFrame = currentConfig.channel_offset;
    
    // Efficient Jump: Only seek if we are not already at the right position
    uint32_t targetPos = frameOffsetBase + startByteInFrame;
    if (fseqFile.position() != targetPos) {
        if (!fseqFile.seek(targetPos)) return false;
    }

    // 3. BLOCK READING
    // We read a 256-byte block. This covers almost all Front/Rear channels 
    // in one single fast hardware access.
    size_t toRead = 256; 
    if (startByteInFrame + toRead > channelCount) {
        toRead = (channelCount > startByteInFrame) ? (channelCount - startByteInFrame) : 0;
    }

    if (toRead > 0) {
        fseqFile.read(frameData, toRead);
    }

    // 4. CALCULATE LED COLORS (Optimized Loop)
    for (size_t i = 0; i < currentConfig.leds.size(); i++) {
        uint16_t rawCh = currentConfig.leds[i].channel;
        
        if (rawCh == 9999) {
            leds[i] = CRGB::Black;
            continue;
        }

        // Fast data fetch from our local block
        uint8_t val = (rawCh < toRead) ? frameData[rawCh] : 0;

        // --- UNIVERSAL S3XY COLOR LOGIC (FRONT & REAR) ---

        // A. AMBER: Indicators (Front 18-23 & Rear 72-77)
        if ((rawCh >= 18 && rawCh <= 23) || (rawCh >= 72 && rawCh <= 77)) {
            leds[i] = CRGB(val, (val * 160) >> 8, 0); 
        }
        // B. PURE WHITE: Beams & Fog (0-11, 24-47) + Reverse (84-87)
        else if ((rawCh >= 0 && rawCh <= 11) || (rawCh >= 24 && rawCh <= 47) || (rawCh >= 84 && rawCh <= 87)) {
            leds[i] = CRGB(val, val, val); 
        }
        // C. COLD WHITE: Signature/DRL (Channels 12-17)
        else if (rawCh >= 12 && rawCh <= 17) {
            leds[i] = CRGB((val * 200) >> 8, (val * 200) >> 8, val); 
        }
        // D. RED: Rear Lights (Tail 60-71, Brake 78-81, CHMSL 93, Fog 96)
        else if ((rawCh >= 60 && rawCh <= 71) || (rawCh >= 78 && rawCh <= 81) || rawCh == 93 || rawCh == 96) {
            leds[i] = CRGB(val, 0, 0);
        }
        // E. FALLBACK: Defaults to Red
        else {
            leds[i] = CRGB(val, 0, 0);
        }
    }

    // 5. OUTPUT
    FastLED.show();

    // Return true if there's a next frame
    return (frameIdx + 1) < frameCount;
}
*/
/*
// with scan reporting
bool playFrame(uint32_t frameIdx) {
    if (!fseqFile || frameIdx >= frameCount) return false;

    // 1. ABSOLUTE POSITIONING (Ignoring JSON Offset for this test)
    // We start at the very beginning of the frame data (Header is 28 bytes)
    uint32_t absoluteFrameStart = 28 + (frameIdx * channelCount);
    if (!fseqFile.seek(absoluteFrameStart)) return false;

    // 2. READ THE FIRST 500 CHANNELS
    uint8_t scanData[500];
    int bytesRead = fseqFile.read(scanData, 500);
    if (bytesRead < 500) return false;

    // 3. TRACK MAXIMUM VALUES (To find active channels)
    static uint8_t globalMax[500];
    for (int i = 0; i < 500; i++) {
        if (scanData[i] > globalMax[i]) {
            globalMax[i] = scanData[i];
        }
    }

    // 4. LIVE LED FEEDBACK (1 LED = 1 Physical Channel)
    // This allows you to see which channels are "dancing" in real-time
    for (int i = 0; i < 32; i++) {
        uint8_t val = scanData[i]; 
        // We show them in white so you can see every flicker
        leds[i] = CRGB(val, val, val); 
    }
    FastLED.show();

    // 5. SERIAL REPORTING (Every 5 seconds / 100 frames)
    if (frameIdx % 100 == 0) {
        Serial.printf("\n--- PHYSICAL SCAN REPORT (Frame %d) ---\n", frameIdx);
        Serial.println("Channels with significant activity (Value > 50):");
        int foundCount = 0;
        for (int i = 0; i < 500; i++) {
            if (globalMax[i] > 50) {
                Serial.printf("[%03d]: %d | ", i, globalMax[i]);
                foundCount++;
                if (foundCount % 6 == 0) Serial.println("");
            }
        }
        if (foundCount == 0) Serial.print("No activity detected in the first 500 channels yet.");
        Serial.println("\n----------------------------------------------");
    }

    return (frameIdx + 1) < frameCount;
}
*/
bool playFrame(uint32_t frameIdx) {
    if (!fseqFile || frameIdx >= frameCount) return false;

    uint32_t targetPos = 28 + (frameIdx * channelCount);
    if (fseqFile.position() != targetPos) {
        if (!fseqFile.seek(targetPos)) return false;
    }

    uint8_t frameData[500]; 
    if (fseqFile.read(frameData, 500) < 1) return false;

    for (size_t i = 0; i < currentConfig.leds.size(); i++) {
        uint16_t rawCh = currentConfig.leds[i].channel;
        if (rawCh == 9999) { leds[i] = CRGB::Black; continue; }

        uint8_t val = (rawCh < 500) ? frameData[rawCh] : 0;

        // --- PRECISION S3XY COLOR LOGIC ---
        
        if (i <= 31) { 
            // ================= FRONT (LED 0-31) =================
            if (rawCh >= 139 && rawCh <= 144) {
                leds[i] = CRGB(val, (val * 160) >> 8, 0); // Front Amber
            } else if (rawCh >= 151 && rawCh <= 160) {
                leds[i] = CRGB((val * 200) >> 8, (val * 200) >> 8, val); // Front Signature
            } else {
                leds[i] = CRGB(val, val, val); // Front Beams (White)
            }
        } 
        else {
            // ================= REAR (LED 32-63) =================
            // 1. REAR AMBER: Blinker (139-144 & 339-344)
            if ((rawCh >= 139 && rawCh <= 144) || (rawCh >= 339 && rawCh <= 344)) {
                leds[i] = CRGB(val, (val * 160) >> 8, 0); 
            }
            // 2. REAR WHITE: Reverse (390-392) & Kennzeichen (184)
            else if ((rawCh >= 390 && rawCh <= 392) || rawCh == 184) {
                leds[i] = CRGB(val, val, val); 
            }
            // 3. REAR RED: Der Rest (Rücklicht, Bremse, Nebelschluss)
            else {
                leds[i] = CRGB(val, 0, 0); 
            }
        }
    }

    FastLED.show();
    return (frameIdx + 1) < frameCount;
}


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
    
    showStatus("READY");
    Serial.println(F("Clean exit."));
}

// ------------------- Web App -------------------
void handleTeslaApp(AsyncWebServerRequest *request) {
    // --- 1. SAFETY CHECK ---
    if (showRunning && request->method() == HTTP_GET) {
        request->send(200, "text/plain", "Show is active. Please check the OLED display.");
        return;
    }

    // --- 2. POST DATA PROCESSING ---
    if (request->method() == HTTP_POST) {
        // Hardware Config
        if (request->hasParam("config", true)) {
            currentConfigFile = "/" + request->getParam("config", true)->value();
            loadConfig(currentConfigFile);
        }

        // Show File
        if (request->hasParam("show", true)) {
            currentShow = "/" + request->getParam("show", true)->value();
            if (fseqFile) fseqFile.close();
            fseqFile = LittleFS.open(currentShow, "r");
            if (fseqFile) readFseqHeader();
        }

        // --- THE NEW INTERNATIONAL TIME LOGIC ---
        // Checks if the phone sent an absolute UTC timestamp
        if (request->hasParam("utc_target", true)) {
            long target = request->getParam("utc_target", true)->value().toInt();
            if (target > 0) {
                showStartEpoch = target;
                triggerCountdown = true;
            }
        } 
        // Fallback for "NOW" button
        if (request->hasParam("instant", true)) {
            showStartEpoch = timeClient.getEpochTime();
            triggerCountdown = true;
        }

        request->redirect("/"); 
        return;
    }

    // --- 3. UI GENERATION ---
    String html = ""; 
    html.reserve(9000);

    // Header & CSS
    html += R"=====(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>S3XY Controller</title><style>
:root { --tesla-red: #cc0000; --tesla-green: #2e7d32; --bg-dark: #121212; --card-bg: #1e1e1e; }
body { font-family: 'Segoe UI', sans-serif; text-align: center; margin: 0; background: var(--bg-dark); color: #e0e0e0; padding: 15px; }
.card { background: var(--card-bg); border-radius: 12px; padding: 20px; margin-bottom: 20px; max-width: 480px; margin-left: auto; margin-right: auto; border: 1px solid #333; box-shadow: 0 4px 15px rgba(0,0,0,0.5); }
h1 { color: var(--tesla-red); letter-spacing: 1px; margin-bottom: 5px; }
h3 { border-bottom: 1px solid #333; padding-bottom: 10px; margin-top: 0; font-size: 1.1em; color: #bbb; text-transform: uppercase; }
label { display: block; text-align: left; font-size: 0.85em; color: #888; margin: 10px 0 5px 0; }
select, input, button { font-size: 16px; padding: 12px; margin: 5px 0; width: 100%; border-radius: 8px; border: 1px solid #333; background: #2a2a2a; color: white; box-sizing: border-box; outline: none; }
select { appearance: none; background-image: url("data:image/svg+xml;charset=US-ASCII,%3Csvg%20xmlns%3D%22http%3A%2F%2Fwww.w3.org%2F2000%2Fsvg%22%20width%3D%22292.4%22%20height%3D%22292.4%22%3E%3Cpath%20fill%3D%22%23FFFFFF%22%20d%3D%22M287%2069.4a17.6%2017.6%200%200%200-13-5.4H18.4c-5%200-9.3%201.8-12.9%205.4A17.6%2017.6%200%200%200%200%2082.2c0%205%201.8%209.3%205.4%2012.9l128%20127.9c3.6%203.6%207.8%205.4%2012.8%205.4s9.2-1.8%2012.8-5.4L287%2095c3.5-3.5%205.4-7.8%205.4-12.8%200-5-1.9-9.2-5.5-12.8z%22%2F%3E%3C%2Fsvg%3E"); background-repeat: no-repeat; background-position: right 12px center; background-size: 12px auto; padding-right: 35px; }
button { background: var(--tesla-red); cursor: pointer; font-weight: bold; border: none; }
.btn-now { background: var(--tesla-green); width: auto !important; padding: 12px 25px !important; margin-left: 5px; text-transform: uppercase; }
.file-list { text-align: left; list-style: none; padding: 0; }
.file-item { padding: 12px; border-bottom: 1px solid #252525; position: relative; }
.btn-del { color: #ff4444; text-decoration: none; font-size: 11px; border: 1px solid #ff4444; padding: 3px 8px; border-radius: 4px; position: absolute; right: 10px; top: 12px; }
.status-pill { display: inline-block; padding: 6px 18px; border-radius: 20px; font-weight: bold; margin-bottom: 20px; font-size: 0.9em; }
</style></head><body><h1>S3XY Lightshow</h1>)=====";

    // --- UI: Status Pill ---
    html += "<div id='status-pill' class='status-pill' style='background:#444;'>LOADING STATUS...</div>";

    if (showRunning || showStartEpoch > 0) {
        html += "<div class='card' style='border-top: 4px solid #f44336;'><h3>Actions</h3><a href='/cancel' style='display:block; background:#d32f2f; color:white; padding:15px; text-decoration:none; border-radius:8px; font-weight:bold;'>STOP / CANCEL SHOW</a></div>";
    }

    // --- UI: Scheduler ---
    html += R"=====(
<div class="card"><h3>Schedule Show</h3><form action="/setshow" method="post">
<input type="hidden" id="utc_target" name="utc_target" value="0">
<label>1. Select Sequence File:</label><select name="show">)=====";

    File root = LittleFS.open("/");
    File file = root.openNextFile();
    while (file) {
        String n = file.name(); if (n.startsWith("/")) n = n.substring(1);
        if (n.endsWith(".fseq")) {
            String selected = (("/"+n) == currentShow) ? " selected" : "";
            html += "<option value='" + n + "'" + selected + ">" + n + "</option>";
        }
        file.close(); file = root.openNextFile();
    }

    html += "</select><label>2. Start Time & Launch:</label><div style='display: flex; gap: 10px;'>";
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
    
    // International Start Button
    html += "<button type='submit' onclick='calculateUTC()' style='background:#444; margin-top:15px; margin-bottom:15px;'>START COUNTDOWN</button>";

    // Advanced Hardware Config
    html += "<hr style='border:0; border-top:1px solid #333; margin:10px 0;'><label>Advanced: Hardware Config</label><select name='config' style='font-size: 14px;'>";
    root = LittleFS.open("/");
    file = root.openNextFile();
    while (file) {
        String n = file.name(); if (n.startsWith("/")) n = n.substring(1);
        if (n.startsWith("config_") && n.endsWith(".json")) {
            String selected = (("/"+n) == currentConfigFile) ? " selected" : "";
            html += "<option value='" + n + "'" + selected + ">" + n + "</option>";
        }
        file.close(); file = root.openNextFile();
    }
    html += "</select></form></div>";

    // --- UI: File Explorer ---
    html += "<div class='card'><h3>Storage Explorer</h3><ul class='file-list'>";
    root = LittleFS.open("/"); 
    file = root.openNextFile();
    while (file) {
        String n = file.name(); if (n.startsWith("/")) n = n.substring(1);
        html += "<li class='file-item'><strong>" + n + "</strong>";
        html += "<a href='/delete?file=" + n + "' class='btn-del' onclick='return confirm(\"Delete permanently?\")'>DELETE</a></li>";
        file.close(); file = root.openNextFile();
    }
    root.close(); 

    // --- UI: Upload ---
    html += R"=====(
        </ul><hr style="border:0; border-top:1px solid #333; margin:20px 0;">
        <label>Upload new Config (.json) or Show (.fseq):</label>
        <form method="POST" action="/upload" enctype="multipart/form-data" style="text-align:left;">
            <input type="file" name="upload" accept=".json,.fseq" style="font-size:12px; border:1px dashed #555; background:transparent; width:100%;">
            <button type="submit" style="background:#444; margin-top:10px; font-size:14px;">UPLOAD FILE</button>
        </form>
        <p><a href="/update" style="color:#388e3c; font-size:11px; text-decoration:none;">&bull; Firmware OTA Portal</a></p>
    </div>
)====="; 

    // --- JAVASCRIPT: International Logic ---
    html += "<script>";
    html += "var targetEpoch = " + String(showStartEpoch) + ";";
    html += "var isRunning = " + String(showRunning ? "true" : "false") + ";";
    html += "var configName = '" + currentConfigFile.substring(currentConfigFile.lastIndexOf('/') + 1) + "';";
    html += R"=====(
    function calculateUTC() {
        var timeVal = document.getElementsByName("start_time")[0].value;
        var h = parseInt(timeVal.split(":")[0]);
        var m = parseInt(timeVal.split(":")[1]);
        var target = new Date();
        target.setHours(h, m, 0, 0);
        if (target.getTime() < Date.now()) target.setDate(target.getDate() + 1);
        document.getElementById('utc_target').value = Math.floor(target.getTime() / 1000);
    }

    function updateCountdown() {
        var now = Math.floor(Date.now() / 1000);
        var pill = document.getElementById('status-pill');
        if (isRunning) {
            pill.innerHTML = "🔴 SHOW ACTIVE (" + configName + ")";
            pill.style.background = "#d32f2f"; return;
        }
        if (targetEpoch > 0) {
            var diff = targetEpoch - now;
            if (diff > 0) {
                pill.innerHTML = "⏳ START IN " + diff + " SECONDS";
                pill.style.background = "#f57c00";
            } else {
                pill.innerHTML = "🚀 SHOW STARTED...";
                pill.style.background = "#388e3c";
                setTimeout(function(){ location.reload(); }, 2000);
            }
        } else {
            pill.innerHTML = "🟢 READY (" + configName + ")";
            pill.style.background = "#388e3c";
        }
    }
    setInterval(updateCountdown, 1000);
    updateCountdown();
    </script></body></html>)=====";

    request->send(200, "text/html", html);
}

void handleDelete(AsyncWebServerRequest *request) {
    if (request->hasParam("file")) {
        String filename = "/" + request->getParam("file")->value();
        
        if (LittleFS.exists(filename)) {
            LittleFS.remove(filename);
            request->send(200, "text/html", "File deleted. <a href='/'>Back</a>");
            Serial.println("Deleted: " + filename);
        } else {
            request->send(404, "text/plain", "File not found.");
        }
    }
}

void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final);

// ------------------- setup & loop -------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== myS3XY Lightshow starting ===");

  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, HIGH); // blue LED off = WiFi not connected

  FastLED.addLeds<WS2812B, 10, GRB>(leds, 64);  // Fixed pin 10, buffer 100 LEDs
  applyPowerSettings();

  u8g2.begin();
  u8g2.setContrast(255);
  u8g2.setBusClock(400000);

  showStatus("Booting...");

  WiFi.begin(ssid, password);
  showStatus("Connecting WiFi...");
  while (WiFi.status() != WL_CONNECTED) delay(500);
  digitalWrite(STATUS_LED, LOW); // blue LED on = WiFi connected
  showStatus("WiFi connected");
  showIP();

  if (MDNS.begin("mys3xy")) {
    Serial.println("mDNS started: mys3xy.local");
    MDNS.addService("http", "tcp", 80);
  }

  timeClient.begin();
  showStatus("Syncing time...");
  bool ntpSuccess = false;
  int attempts = 0;
  while (attempts < 20 && !ntpSuccess) {
    if (timeClient.update() || timeClient.forceUpdate()) ntpSuccess = true;
    attempts++;
    delay(500);
  }
  if (ntpSuccess) showStatus("Time synced");
  else {
    showStatus("Time failed");
    delay(2000);
  }

  if (!LittleFS.begin(true)) { 
    Serial.println("LittleFS Error!");
    showStatus("FS Format..."); 
  }
  Serial.println("LittleFS mounted");

  // List all files
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    Serial.println("Found file: " + String(file.name()));
    file = root.openNextFile();
  }
  root.close();

  loadConfig(currentConfigFile);

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
  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request) {
    String filename = "";
    if (request->hasParam("file")) {
        filename = request->getParam("file")->value();
        if (!filename.startsWith("/")) filename = "/" + filename;
        LittleFS.remove(filename);
        Serial.printf("File deleted: %s\n", filename.c_str());
    }

    // Build Response in the same style as Upload
    String statusColor = "#cc0000"; // Tesla Red for Delete
    String html = "<html><head>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{font-family:Arial;text-align:center;background:#121212;color:white;padding:20px;}";
    html += ".box{background:#1e1e1e;padding:30px;border-radius:12px;border-top:5px solid " + statusColor + ";display:inline-block;width:90%;max-width:400px;margin-top:50px;}";
    html += "a{display:block;background:#333;color:white;padding:15px;text-decoration:none;border-radius:6px;margin-top:20px;font-weight:bold;transition:0.3s;}";
    html += "a:hover{background:#444;}</style></head><body><div class='box'>";

    html += "<h2>File Deleted</h2>";
    html += "<p style='color:#888;'>The following file has been removed:</p>";
    html += "<p style='font-family:monospace;background:#252525;padding:10px;border-radius:4px;'>" + filename + "</p>";

    html += "<a href='/'>[ Back to Dashboard ]</a>";
    html += "</div></body></html>";

    request->send(200, "text/html", html);
    });
  server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *request) {
      bool isValid = true;
      String message = "Upload successful!";
      
      if (lastUploadedFilename.endsWith(".json")) {
          File file = LittleFS.open("/" + lastUploadedFilename, "r");
          if (file) {
              JsonDocument doc;
              DeserializationError error = deserializeJson(doc, file);
              file.close();
              if (error) {
                  isValid = false;
                  message = "JSON ERROR: " + String(error.c_str());
                  LittleFS.remove("/" + lastUploadedFilename);
              }
          }
      }

      String statusColor = isValid ? "#4CAF50" : "#f44336";
      String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'></head>";
      html += "<body style='font-family:Arial;text-align:center;background:#121212;color:white;padding:20px;'>";
      html += "<div style='background:#1e1e1e;padding:30px;border-radius:12px;border-top:5px solid " + statusColor + ";display:inline-block;width:90%;max-width:400px;'>";
      html += "<h2>" + message + "</h2>";
      html += "<p>File: " + lastUploadedFilename + "</p>";
      html += "<br><a href='/' style='display:block;background:#cc0000;color:white;padding:15px;text-decoration:none;border-radius:6px;font-weight:bold;'>[ Back to Dashboard ]</a>";
      html += "</div></body></html>";
      
      request->send(200, "text/html", html);
  }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (!index) {
          if (filename.startsWith("/")) lastUploadedFilename = filename.substring(1);
          else lastUploadedFilename = filename;
          request->_tempFile = LittleFS.open("/" + lastUploadedFilename, "w");
      }
      if (len && request->_tempFile) {
          request->_tempFile.write(data, len);
          // WICHTIG: Nach jedem Block kurz Zeit für das System lassen
          yield(); 
      }
      if (final && request->_tempFile) {
          request->_tempFile.close();
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

void loop() {
  ElegantOTA.loop();
  timeClient.update();
  unsigned long currentEpoch = timeClient.getEpochTime();

  // CASE 1: Waiting for scheduled start (Countdown)
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
    } else {
    // --- START SHOW NOW ---
    isBusy = true; // Block webserver for a moment during file opening
    if (fseqFile) { fseqFile.close(); fseqFile = File(); } 

    fseqFile = LittleFS.open(currentShow, "r");
    if (fseqFile && readFseqHeader()) {
        showRunning = true;
        currentFrame = 0; 
        
        // SYNC: Start the precise millisecond timer
        showStartTimeMillis = millis(); 
        showStartEpoch = currentEpoch; // Keep this for the Web UI status
        
        // STATIC OLED: Update once, then leave it alone during playback
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_logisoso24_tf); // Slightly smaller to fit more text if needed
        u8g2.drawStr(xOffset, yOffset + 45, "ACTIVE");
        u8g2.sendBuffer();
        
        Serial.println(F("Show started: Precise sync active, OLED silenced."));
    } else {
        stopShowAndCleanup();
    }
      isBusy = false;
    }
  }

  // CASE 2: Show is active

    if (showRunning) {
        // --- PERFORMANCE MONITOR VARIABLES ---
        static uint32_t totalProcessTime = 0;
        static uint16_t sampleCounter = 0;
        
        // 1. HIGH PRECISION TIMING
        unsigned long msElapsed = millis() - showStartTimeMillis;
        uint32_t targetFrame = msElapsed / stepTimeMs;

        // 2. PLAYBACK LOGIC
        if (targetFrame >= currentFrame) {
            // Check for lag
            if (targetFrame > currentFrame + 2) {
                currentFrame = targetFrame;
            }

            // --- START MEASURING PLAYFRAME ---
            unsigned long startMicros = micros();
            
            if (!playFrame(currentFrame)) {
                stopShowAndCleanup();
            } else {
                currentFrame++;
            }

            // --- CALCULATE DURATION ---
            uint32_t duration = (micros() - startMicros) / 1000; // Time in milliseconds
            totalProcessTime += duration;
            sampleCounter++;

            // Report average every 100 frames (approx. 5 seconds)
            if (sampleCounter >= 100) {
                uint32_t avg = totalProcessTime / 100;
                Serial.printf(">>> PERFORMANCE: Avg Frame Time %d ms | Target: %d ms\n", avg, stepTimeMs);
                
                // Warning if we are too slow
                if (avg >= stepTimeMs) {
                    Serial.println("!!! WARNING: Storage or CPU too slow for this show!");
                }
                
                totalProcessTime = 0;
                sampleCounter = 0;
            }
        }
    }
}