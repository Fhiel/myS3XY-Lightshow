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

// OLED (visible area 72x40 in 128x64 buffer)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE, /* clock=*/ 6, /* data=*/ 5);

const int width = 72;
const int height = 40;
const int xOffset = 28;  // (128 - 72) / 2
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
String currentConfigFile = "/config_full.json";  // Default config

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
  u8g2.drawStr(xOffset, yOffset + 15, "IP:");
  String ip = WiFi.localIP().toString();
  u8g2.drawStr(xOffset, yOffset + 30, ip.c_str());
  u8g2.drawStr(xOffset, yOffset + 45, "mys3xy.local");
  u8g2.sendBuffer();
  delay(5000);
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

void teslaCountdown() {
  while (true) {
    timeClient.update();
    unsigned long currentEpoch = timeClient.getEpochTime();
    long secondsLeft = showStartEpoch - currentEpoch;

    if (secondsLeft <= 0) {
      showRunning = true;
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_logisoso32_tr);
      u8g2.drawStr(xOffset + 15, yOffset + 38, "GO!");
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
      u8g2.drawStr(xOffset + 5, yOffset + 35, buf);
    } else {
      sprintf(buf, "%02d", secs);
      u8g2.setFont(u8g2_font_logisoso42_tr);
      u8g2.drawStr(xOffset + 10, yOffset + 38, buf);
    }

    u8g2.sendBuffer();
    delay(200);
  }
}

// ------------------- Web App -------------------
void handleTeslaApp(AsyncWebServerRequest *request) {
  if (request->method() == HTTP_POST) {
    bool configChanged = false;

    if (request->hasParam("config", true)) {
      currentConfigFile = "/" + request->getParam("config", true)->value();
      if (loadConfig(currentConfigFile)) {
        FastLED.clear();
        FastLED.addLeds<WS2812B, 10, GRB>(leds, 50);  // Fixed pin 10, buffer 50
        configChanged = true;
      }
    }

    if (request->hasParam("start_time", true)) {
      String timeStr = request->getParam("start_time", true)->value();
      int hour = timeStr.substring(0,2).toInt();
      int minute = timeStr.substring(3,5).toInt();

      unsigned long currentEpoch = timeClient.getEpochTime();
      struct tm *ptm = gmtime((time_t*)&currentEpoch);
      ptm->tm_hour = hour;
      ptm->tm_min = minute;
      ptm->tm_sec = 0;

      showStartEpoch = mktime(ptm);

      if (showStartEpoch > currentEpoch + 600) {
        showStartEpoch = currentEpoch + 600;
      }

      String response = "<h2>Show scheduled!</h2><p>Start: " + timeStr + "</p>";
      if (configChanged) response += "<p>Config: " + currentConfig.name + "</p>";
      response += "<a href='/'>Back</a>";

      request->send(200, "text/html", response);
      teslaCountdown();
      return;
    }
  }

  // GET: Show web app
  String html = R"=====(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>myS3XY Lightshow</title>
  <style>
    body { font-family: Arial; text-align: center; margin: 40px; background: #000; color: #fff; }
    select, input, button { font-size: 20px; padding: 10px; margin: 10px; width: 80%; }
    button { background: #f00; color: white; border: none; }
  </style>
</head>
<body>
  <h1>myS3XY Lightshow</h1>

  <form action="/setshow" method="post">
    <p><strong>Select config</strong></p>
    <select name="config">
)=====";

  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String filename = file.name();
    if (filename.startsWith("/config_") && filename.endsWith(".json")) {
      String configName = filename.substring(1);
      String displayName = configName.substring(8, configName.length() - 5);
      html += "<option value='" + configName + "'>" + displayName + "</option>";
    }
    file = root.openNextFile();
  }
  root.close();

  html += R"=====(
    </select>

    <p><strong>Start time (max 10 min)</strong></p>
    <input type="time" name="start_time" required>

    <br><br>
    <button type="submit">START SHOW</button>
  </form>

  <hr>
  <p><a href="/update">Upload new config/show</a></p>
</body>
</html>
)=====";

  request->send(200, "text/html", html);
}

// ------------------- setup & loop -------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== myS3XY Lightshow starting ===");

  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);

  FastLED.addLeds<WS2812B, 10, GRB>(leds, 50);  // Fixed pin 10, buffer 50 LEDs
  FastLED.setBrightness(128);

  u8g2.begin();
  u8g2.setContrast(255);
  u8g2.setBusClock(400000);

  showStatus("Booting...");

  WiFi.begin(ssid, password);
  showStatus("Connecting WiFi...");
  while (WiFi.status() != WL_CONNECTED) delay(500);
  digitalWrite(STATUS_LED, HIGH);
  showStatus("WiFi connected");
  showIP();

  if (MDNS.begin("myS3XY")) {
    Serial.println("mDNS started: myS3XY.local");
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
    showStatus("Filesystem error");
    while (1);
  }

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

  server.begin();
  Serial.println("Web server & OTA ready");
  showStatus("App ready");
}

void loop() {
  ElegantOTA.loop();

  if (!showRunning) return;

  timeClient.update();
  unsigned long currentEpoch = timeClient.getEpochTime();
  unsigned long elapsedSeconds = currentEpoch - showStartEpoch;
  unsigned long targetFrame = elapsedSeconds * 1000 / stepTimeMs;

  static uint32_t currentFrame = 0;

  if (targetFrame > currentFrame) {
    if (!playFrame(currentFrame)) {
      showRunning = false;
      showStatus("SHOW DONE");
      FastLED.clear();
      FastLED.show();
      return;
    }
    currentFrame++;

    static unsigned long lastUpdate = 0;
    if (millis() - lastUpdate > 500) {
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x10_tr);
      u8g2.drawStr(xOffset, yOffset + 10, "Frame:");
      char buf[20];
      sprintf(buf, "%lu/%lu", currentFrame, frameCount);
      u8g2.drawStr(xOffset, yOffset + 25, buf);
      u8g2.sendBuffer();
      lastUpdate = millis();
    }
  }
  delay(1);
}