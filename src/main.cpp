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

// OLED (visible area 72x40 in 132x64 buffer)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ 6, /* data=*/ 5);

const int width = 72;
const int height = 40;
const int xOffset = 30;  // (132 - 72) / 2
const int yOffset = 12;  // (64 - 40) / 2

// -----------------------------------------------------

CRGB leds[50];  // Buffer for max 50 LEDs (adjusted dynamically by config)

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600, 60000);  // UTC+1 (CET)

AsyncWebServer server(80);

File fseqFile;
uint32_t channelCount = 0;
uint32_t frameCount = 0;
uint16_t stepTimeMs = 50;

unsigned long showStartEpoch = 0;
bool showRunning = false;
bool triggerCountdown = false;  // New: Signal to start the countdown
unsigned long lastOledUpdate = 0;
uint32_t currentFrame = 0; // Track current frame globally

// ------------------- Config Structure -------------------
struct LedMapping {
  uint16_t channel;
};

struct Config {
  String name = "Default";
  uint8_t data_pin = DATA_PIN;
  uint16_t channel_offset = 0;
  std::vector<LedMapping> leds;
};

Config currentConfig;
String currentConfigFile = "/test.json";  // Default config
String currentShow = "/show.fseq";  // Default FSEQ-file

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

bool loadConfig(const String& filename) {
  File file = LittleFS.open(filename, "r");
  if (!file) {
    Serial.println("Config " + filename + " not found");
    return false;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.println("JSON error in " + filename + ": " + error.c_str());
    return false;
  }

  currentConfig.name = doc["name"] | "Unknown";
  currentConfig.data_pin = doc["data_pin"] | DATA_PIN;
  currentConfig.channel_offset = doc["channel_offset"] | 0;

  currentConfig.leds.clear();
  JsonArray arr = doc["leds"];
  for (JsonObject obj : arr) {
    LedMapping m;
    m.channel = obj["channel"] | 0;
    currentConfig.leds.push_back(m);
  }

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
  if (!fseqFile) return false;

  uint8_t header[28];
  fseqFile.seek(0);
  if (fseqFile.read(header, 28) != 28) return false;

  channelCount = header[10] | (header[11] << 8) | (header[12] << 16) | (header[13] << 24);
  frameCount   = header[14] | (header[15] << 8) | (header[16] << 16) | (header[17] << 24);
  stepTimeMs   = header[18] | (header[19] << 8);

  Serial.printf("FSEQ loaded: %lu channels, %lu frames, %u ms/step\n", channelCount, frameCount, stepTimeMs);
  return true;
}

bool playFrame(uint32_t frameIdx) {
  // Prüfen, ob die aktuelle FSEQ-Datei die richtige ist
  if (!fseqFile || currentShow != fseqFile.name()) {
    if (fseqFile) {
      fseqFile.close();
      Serial.println("Closed old FSEQ");
    }
    fseqFile = LittleFS.open(currentShow, "r");
    if (!fseqFile) {
      Serial.println("ERROR: Cannot open " + currentShow);
      showStatus("FSEQ error");
      return false;
    }
    Serial.println("Opened new FSEQ: " + currentShow);
    if (!readFseqHeader()) {
      Serial.println("ERROR: Failed to read FSEQ header");
      return false;
    }
  }

  uint32_t frameOffsetBase = 28 + frameIdx * channelCount;

  for (size_t i = 0; i < currentConfig.leds.size(); i++) {
    uint32_t ch = currentConfig.leds[i].channel + currentConfig.channel_offset;
    uint32_t offset = frameOffsetBase + ch;

    if (offset + 2 >= fseqFile.size()) {
      leds[i] = CRGB::Black;
      continue;
    }

    fseqFile.seek(offset);
    uint8_t r, g, b;
    fseqFile.read(&r, 1);
    fseqFile.read(&g, 1);
    fseqFile.read(&b, 1);
    leds[i] = CRGB(r, g, b);
  }

  FastLED.show();
  return (frameIdx + 1) < frameCount;
}

/* void teslaCountdown() {
  while (true) {
    timeClient.update();
    unsigned long currentEpoch = timeClient.getEpochTime();
    long secondsLeft = showStartEpoch - currentEpoch;

    if (secondsLeft <= 0) {
      showRunning = true;
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_logisoso32_tr);
      u8g2.drawStr(xOffset + 15, yOffset + 44, "GO!");
      u8g2.sendBuffer();
      delay(1000);
      return;
    }

    int mins = secondsLeft / 60;
    int secs = secondsLeft % 60;

    char buf[6];
    u8g2.clearBuffer();

    if (mins > 0) {
      sprintf(buf, "%02d:%02d", mins, secs);
      u8g2.setFont(u8g2_font_logisoso24_tr);
      u8g2.drawStr(xOffset, yOffset + 34, buf);
    } else {
      sprintf(buf, "%02d", secs);
      u8g2.setFont(u8g2_font_logisoso38_tr);
      u8g2.drawStr(xOffset + 15, yOffset + 44, buf);
    }

    u8g2.sendBuffer();
    delay(200);
  }
} */

// ------------------- Web App -------------------
void handleTeslaApp(AsyncWebServerRequest *request) {
    if (request->method() == HTTP_POST) {
        bool configChanged = false;

        // 1. Handle Config Selection
        if (request->hasParam("config", true)) {
            currentConfigFile = "/" + request->getParam("config", true)->value();
            if (loadConfig(currentConfigFile)) {
                FastLED.clear();
                // Re-initialize with potentially new config parameters
                FastLED.addLeds<WS2812B, 10, GRB>(leds, 50); 
                configChanged = true;
            }
        }

        // 2. Handle Show Selection
        if (request->hasParam("show", true)) {
            currentShow = "/" + request->getParam("show", true)->value();
            if (fseqFile) fseqFile.close();
            fseqFile = LittleFS.open(currentShow, "r");
            if (fseqFile) {
                readFseqHeader();
            }
        }

        // 3. Handle Schedule Start
        if (request->hasParam("start_time", true)) {
            // 1. Get parameters from the request
            String timeStr = request->getParam("start_time", true)->value();
            String selectedConfig = request->hasParam("config", true) ? request->getParam("config", true)->value() : "Default";
            String selectedShow = request->hasParam("show", true) ? request->getParam("show", true)->value() : "None";

            // 2. Calculate time (your existing logic)
            int hour = timeStr.substring(0,2).toInt();
            int minute = timeStr.substring(3,5).toInt();
            unsigned long currentEpoch = timeClient.getEpochTime();
            struct tm *ptm = gmtime((time_t*)&currentEpoch);
            ptm->tm_hour = hour;
            ptm->tm_min = minute;
            ptm->tm_sec = 0;
            showStartEpoch = mktime(ptm);

            if (showStartEpoch > currentEpoch + 600) showStartEpoch = currentEpoch + 600;

            // 3. Build a beautiful response page
            String response = R"=====(
        <!DOCTYPE html>
        <html>
        <head>
            <meta charset="UTF-8">
            <meta name="viewport" content="width=device-width, initial-scale=1">
            <meta http-equiv="refresh" content="5;url=/">
            <title>myS3XY - Scheduled</title>
            <style>
                body { font-family: 'Segoe UI', sans-serif; text-align: center; background: #121212; color: #e0e0e0; padding: 20px; }
                .card { background: #1e1e1e; border-radius: 12px; padding: 30px; box-shadow: 0 4px 15px rgba(0,0,0,0.5); max-width: 400px; margin: 50px auto; border-top: 5px solid #cc0000; }
                h1 { color: #cc0000; letter-spacing: 2px; margin-bottom: 5px; }
                .success-icon { font-size: 50px; color: #4CAF50; margin: 20px 0; }
                .info { background: #2a2a2a; padding: 15px; border-radius: 8px; text-align: left; margin: 20px 0; }
                .info p { margin: 5px 0; font-size: 16px; }
                .footer { font-size: 12px; color: #888; }
                a { color: #cc0000; text-decoration: none; font-weight: bold; }
            </style>
        </head>
        <body>
            <h1>myS3XY Tesla Show</h1>
            <div class="card">
                <div class="success-icon">✔</div>
                <h2>Show Scheduled!</h2>
                
                <div class="info">
        )=====";

            response += "<p><strong>⏰ Start Time:</strong> " + timeStr + "</p>";
            response += "<p><strong>🛠 Config:</strong> " + selectedConfig + "</p>";
            response += "<p><strong>🎬 Show:</strong> " + selectedShow + "</p>";
            
            response += R"=====(
                </div>
                
                <p class="footer">Redirecting to dashboard in 5 seconds...</p>
                <p><a href="/">[ Back Now ]</a></p>
            </div>
        </body>
        </html>
        )=====";

            request->send(200, "text/html", response);
            triggerCountdown = true;
            return;
        }
    }

    // --- Build Web Interface (GET) ---
    
    // Header & CSS
    String html = R"=====(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>my S3XY Lightshow</title>
    <style>
        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; text-align: center; margin: 0; background: #121212; color: #e0e0e0; padding: 20px; }
        .card { background: #1e1e1e; border-radius: 12px; padding: 20px; margin-bottom: 20px; box-shadow: 0 4px 15px rgba(0,0,0,0.5); max-width: 500px; margin-left: auto; margin-right: auto; }
        h1 { color: #cc0000; letter-spacing: 2px; }
        h3 { border-bottom: 1px solid #333; padding-bottom: 10px; color: #f0f0f0; }
        select, input, button { font-size: 18px; padding: 12px; margin: 10px 0; width: 100%; border-radius: 6px; border: 1px solid #333; background: #2a2a2a; color: white; box-sizing: border-box; }
        button { background: #cc0000; color: white; border: none; cursor: pointer; font-weight: bold; margin-top: 20px; }
        button:hover { background: #ff0000; }
        .file-list { text-align: left; list-style: none; padding: 0; }
        .file-item { display: flex; justify-content: space-between; align-items: center; padding: 8px; border-bottom: 1px solid #333; font-family: monospace; }
        .btn-del { color: #ff4444; text-decoration: none; font-weight: bold; padding: 5px 10px; border: 1px solid #ff4444; border-radius: 4px; font-size: 12px; }
        .btn-del:hover { background: #ff4444; color: white; }
        .sys-tag { color: #666; font-size: 12px; font-style: italic; }
        .storage-info { font-size: 12px; color: #888; margin-top: 10px; }
    </style>
    <script>
        // Prüft alle 2 Sekunden, ob die Seite neu geladen werden muss
        setInterval(function() {
            var statusText = document.getElementById('status-bar').innerText;
            // Wenn ein Countdown (⏳) oder eine Show (🔴) aktiv ist, lade neu
            if (statusText.includes("⏳") || statusText.includes("🔴")) {
                location.reload();
            }
        }, 2000);
    </script>
</head>
<body>
    <h1>my S3XY Lightshow</h1>
)=====";
    // Status Indicator
    String status = getSystemStatus();
    String color = "#4CAF50"; // Green
    if (status.indexOf("🔴") >= 0) color = "#f44336"; // Red (Note: indexOf is safer than startsWith for emojis)
    if (status.indexOf("⏳") >= 0) color = "#ff9800"; // Orange
    
    html += "<div id='status-bar' style='display:inline-block; padding: 5px 15px; border-radius: 20px; background:" + color + "; color:white; font-weight:bold; margin-bottom:20px; font-size:14px;'>";
    html += status;
    html += "</div>";

    html += R"=====(
    <div class="card">
        <h3>Show Settings</h3>
        <form action="/setshow" method="post">
            <label>Select Configuration:</label>
            <select name="config">
)=====";

    // --- Populate Config Dropdown ---
    File root = LittleFS.open("/");
    if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file) {
            String n = file.name();
            if (n.startsWith("/")) n = n.substring(1);
            if (n.startsWith("config_") && n.endsWith(".json")) {
                String selected = (n == "config_full.json") ? " selected" : "";
                html += "<option value='" + n + "'" + selected + ">" + n + "</option>";
            }
            file.close();
            file = root.openNextFile();
        }
        root.close();
    }

    html += R"=====(
            </select>

            <label>Select Show File (.fseq):</label>
            <select name="show">
)=====";

    // --- Populate Show Dropdown ---
    root = LittleFS.open("/");
    if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file) {
            String n = file.name();
            if (n.startsWith("/")) n = n.substring(1);
            if (n.endsWith(".fseq")) {
                String selected = (n == "show.fseq") ? " selected" : "";
                html += "<option value='" + n + "'" + selected + ">" + n + "</option>";
            }
            file.close();
            file = root.openNextFile();
        }
        root.close();
    }

    html += R"=====(
            </select>

            <label>Start Time:</label>
            <input type="time" name="start_time" required>

            <button type="submit">START COUNTDOWN</button>
        </form>
    </div>

    <div class="card">
        <h3>File Explorer</h3>
        <p class="storage-info">)=====";
    
    html += getStorageInfo(); // Your helper function
    
    html += "</p><ul class='file-list'>";

    // --- Build File Explorer List with Delete Buttons ---
    root = LittleFS.open("/");
    if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file) {
            String n = file.name();
            if (n.startsWith("/")) n = n.substring(1);
            
            html += "<li class='file-item'><span>" + n + "</span>";
            
            if (n != "config_full.json" && n != "show.fseq") {
                html += "<a href='/delete?file=" + n + "' class='btn-del' onclick='return confirm(\"Delete this file?\")'>DELETE</a>";
            } else {
                html += "<span class='sys-tag'>[System File]</span>";
            }
            html += "</li>";

            File nextFile = root.openNextFile();
            file.close();
            file = nextFile;
        }
        root.close();
    }

    html += R"=====(
        </ul>
        <hr style="border:0; border-top:1px solid #333; margin:20px 0;">
        <p><a href="/update" style="color:#00ff00; text-decoration:none;">&rarr; Upload New Files (OTA)</a></p>
    </div>
</body>
</html>
)=====";

    request->send(200, "text/html", html);
}

void handleDelete(AsyncWebServerRequest *request) {
    if (request->hasParam("file")) {
        String filename = "/" + request->getParam("file")->value();
        
        // Securtiy check: protect default files
        if (filename == "/config_full.json" || filename == "/show.fseq") {
            request->send(403, "text/plain", "Error: You can't delete default files!");
            return;
        }

        if (LittleFS.exists(filename)) {
            LittleFS.remove(filename);
            request->send(200, "text/html", "File deleted. <a href='/'>Back</a>");
            Serial.println("Deleted: " + filename);
        } else {
            request->send(404, "text/plain", "File not found.");
        }
    }
}

// ------------------- setup & loop -------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== myS3XY Lightshow starting ===");

  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, HIGH); // blue LED off

  FastLED.addLeds<WS2812B, 10, GRB>(leds, 50);  // Fixed pin 10, buffer 50 LEDs
  FastLED.setBrightness(128);

  u8g2.begin();
  u8g2.setContrast(255);
  u8g2.setBusClock(400000);

  showStatus("Booting...");

  WiFi.begin(ssid, password);
  showStatus("Connecting WiFi...");
  while (WiFi.status() != WL_CONNECTED) delay(500);
  digitalWrite(STATUS_LED, LOW); // blue LED on
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

  if (!LittleFS.begin()) {
    showStatus("Filesys error");
    while (1);
  }
  Serial.println("LittleFS mounted");

  // Alle Dateien auflisten
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    Serial.println("Found file: " + String(file.name()));
    file = root.openNextFile();
  }
  root.close();

  loadConfig(currentConfigFile);

  if (!LittleFS.exists("/show.fseq")) {
    showStatus("No show.fseq");
    while (1);
  }
  fseqFile = LittleFS.open("/show.fseq", "r");
  readFseqHeader();

  ElegantOTA.begin(&server);
  server.on("/", HTTP_GET, handleTeslaApp);
  server.on("/setshow", HTTP_GET, handleTeslaApp);
  server.on("/setshow", HTTP_POST, handleTeslaApp);
  server.on("/delete", HTTP_GET, handleDelete);

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
      // Display Countdown on OLED
      static unsigned long lastUpdate = 0;
      if (millis() - lastUpdate > 500) { // Update OLED every 500ms
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
      // Countdown reached zero -> Start Show
      triggerCountdown = false;
      showRunning = true;
      currentFrame = 0; 
      
      // IMPORTANT: Sync show start to current network time
      showStartEpoch = currentEpoch; 
      
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_logisoso32_tr);
      u8g2.drawStr(xOffset + 10, yOffset + 48, "GO!");
      u8g2.sendBuffer();
    }
  }
  // CASE 2: Show is active
  if (showRunning) {
    // Calculate how many milliseconds have passed since the "GO!"
    unsigned long millisSinceStart = (timeClient.getEpochTime() - showStartEpoch) * 1000;
    
    // Safety check: ensure stepTimeMs is not zero
    if (stepTimeMs == 0) stepTimeMs = 50; 

    // Determine the target frame based on elapsed time
    uint32_t targetFrame = millisSinceStart / stepTimeMs;

    if (targetFrame >= currentFrame) {
        // Play the current frame
        if (!playFrame(currentFrame)) {
            // If playFrame returns false, the show is over (EOF)
            showRunning = false;
            showStartEpoch = 0;
            triggerCountdown = false;
            currentFrame = 0;
            showStatus("SHOW DONE");
            FastLED.clear(true);
        } else {
            // Move to the next frame
            currentFrame++;
            
            // Optional: Update OLED every 10 frames to save CPU
            if (currentFrame % 10 == 0) {
                u8g2.clearBuffer();
                u8g2.setFont(u8g2_font_6x10_tr);
                char buf[20];
                sprintf(buf, "%lu/%lu", currentFrame, frameCount);
                u8g2.drawStr(xOffset, yOffset + 25, buf);
                u8g2.sendBuffer();
            }
        }
    }
  }
}