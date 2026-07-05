/*
 * TideDisplay.ino
 * ESP32 (ESP-32E) + 3.2" ILI9341 TFT display
 * Shows: Great Wave graphic | Last 4 tide times (H/L) | Current temp & feels-like
 * Location: Hilton Head Island, SC 29928  (NOAA Station 8665530)
 * WiFi: Dexter5G
 * Display: Landscape, USB/connector on LEFT → rotation = 3 (inverted landscape)
 *
 * Libraries needed:
 *   - TFT_eSPI  (configure User_Setup.h for your ILI9341 pinout)
 *   - ArduinoJson
 *   - HTTPClient (built-in ESP32)
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include "wave_img.h"   // RGB565 bitmap array

// ── WiFi credentials ─────────────────────────────────────────────────────────
const char* SSID     = "Dexter5G";
const char* PASSWORD = "madmax";

// ── NOAA tide station: Hilton Head Island / Port Royal Sound ─────────────────
// Station 8665530 – Hilton Head Island, SC
const char* NOAA_STATION = "8665530";

// ── Open-Meteo weather (Hilton Head lat/lon) ──────────────────────────────────
// lat=32.1354, lon=-80.9274
const char* WEATHER_URL =
  "https://api.open-meteo.com/v1/forecast"
  "?latitude=32.1354&longitude=-80.9274"
  "&current=temperature_2m,apparent_temperature,relative_humidity_2m"
  "&temperature_unit=fahrenheit"
  "&wind_speed_unit=mph"
  "&timezone=America%2FNew_York";

// ── Refresh intervals ─────────────────────────────────────────────────────────
const unsigned long TIDE_REFRESH_MS    = 6UL * 3600UL * 1000UL;  // 6 hours
const unsigned long WEATHER_REFRESH_MS = 10UL * 60UL * 1000UL;   // 10 minutes

// ── Display ───────────────────────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();

// Layout constants (320 × 240, landscape)
// Left 66%  = 0..210  (wave image + tide times)
// Right 34% = 211..319 (weather)
const int DIVIDER_X    = 212;
const int WAVE_X       = 2;
const int WAVE_Y       = 52;   // center wave vertically below title
const int TIDE_X       = 104;  // tide text starts after wave image
const int SCREEN_W     = 320;
const int SCREEN_H     = 240;

// ── Colours (RGB565) ──────────────────────────────────────────────────────────
#define COL_BG          0x0000  // black
#define COL_WHITE       0xFFFF
#define COL_OLIVE       0x6CC0  // olive green  ~#6B8000  → (0x6B>>3)<<11|(0x80>>2)<<5|(0x00>>3)
                                // precise: R=13,G=32,B=0 → 0x1B00 ... let's compute properly:
                                // olive #808000: R=128,G=128,B=0 → (16<<11)|(32<<5)|0 = 0x8400
#define COL_LOW_TIDE    0x8400  // olive green
#define COL_HIGH_TIDE   0x34BF  // medium blue #3399FF → R=6,G=25,B=31 = 0x32DF
#define COL_FEELS       0xF800  // bright red
#define COL_DIVIDER     0x2945  // dim grey

// ── Tide data ─────────────────────────────────────────────────────────────────
struct TideEvent {
  char time[12];   // "hh:mm AM"
  char date[10];   // "Mon DD"
  bool isHigh;
};

TideEvent tides[4];
int  tideCount = 0;

// ── Weather data ──────────────────────────────────────────────────────────────
float currentTemp   = 0.0f;
float feelsLikeTemp = 0.0f;
bool  weatherValid  = false;

// ── Timers ────────────────────────────────────────────────────────────────────
unsigned long lastTideFetch    = 0;
unsigned long lastWeatherFetch = 0;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

// Convert "2025-06-28T14:30" → "2:30 PM" and "Jun 28"
void parseNoaaTime(const char* iso, char* timeOut, char* dateOut) {
  // Format: YYYY-MM-DD HH:MM  (NOAA returns space-separated)
  int yr, mo, dy, hr, mn;
  sscanf(iso, "%d-%d-%d %d:%d", &yr, &mo, &dy, &hr, &mn);

  const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                           "Jul","Aug","Sep","Oct","Nov","Dec"};
  snprintf(dateOut, 10, "%s %02d", months[mo-1], dy);

  const char* ampm = (hr >= 12) ? "PM" : "AM";
  int hr12 = hr % 12;
  if (hr12 == 0) hr12 = 12;
  snprintf(timeOut, 12, "%d:%02d %s", hr12, mn, ampm);
}

// ─────────────────────────────────────────────────────────────────────────────
// Fetch tide predictions from NOAA CO-OPS API
// ─────────────────────────────────────────────────────────────────────────────
void fetchTides() {
  // Get today + tomorrow predictions, pick next 4 H/L events
  // NOAA API: https://api.tidesandcurrents.noaa.gov/api/prod/datagetter
  char url[300];
  // Use "date=today" range 2 days to capture next 4 events
  snprintf(url, sizeof(url),
    "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter"
    "?begin_date=today&range=48&station=%s"
    "&product=predictions&datum=MLLW&time_zone=lst_ldt"
    "&interval=hilo&units=english&application=tide_display&format=json",
    NOAA_STATION);

  HTTPClient http;
  http.begin(url);
  http.setTimeout(10000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("NOAA HTTP error: %d\n", code);
    http.end();
    return;
  }

  String body = http.getString();
  http.end();

  // Parse JSON
  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("NOAA JSON error: %s\n", err.c_str());
    return;
  }

  JsonArray predictions = doc["predictions"].as<JsonArray>();
  tideCount = 0;

  for (JsonObject p : predictions) {
    if (tideCount >= 4) break;
    const char* t  = p["t"];   // time string "YYYY-MM-DD HH:MM"
    const char* ty = p["type"];  // "H" or "L"

    parseNoaaTime(t, tides[tideCount].time, tides[tideCount].date);
    tides[tideCount].isHigh = (ty[0] == 'H');
    tideCount++;
  }

  Serial.printf("Fetched %d tide events\n", tideCount);
  lastTideFetch = millis();
}

// ─────────────────────────────────────────────────────────────────────────────
// Fetch weather from Open-Meteo
// ─────────────────────────────────────────────────────────────────────────────
void fetchWeather() {
  HTTPClient http;
  http.begin(WEATHER_URL);
  http.setTimeout(10000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Weather HTTP error: %d\n", code);
    http.end();
    return;
  }

  String body = http.getString();
  http.end();

  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("Weather JSON error: %s\n", err.c_str());
    return;
  }

  currentTemp   = doc["current"]["temperature_2m"].as<float>();
  feelsLikeTemp = doc["current"]["apparent_temperature"].as<float>();
  weatherValid  = true;

  Serial.printf("Weather: %.1f°F, feels like %.1f°F\n", currentTemp, feelsLikeTemp);
  lastWeatherFetch = millis();
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw wave image (RGB565 bitmap)
// ─────────────────────────────────────────────────────────────────────────────
void drawWave() {
  // Center vertically in the left panel
  int y0 = (SCREEN_H - WAVE_H) / 2;
  if (y0 < 0) y0 = 0;
  tft.pushImage(WAVE_X, y0, WAVE_W, WAVE_H, (uint16_t*)wave_img);
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw tide section (right of wave, left of divider)
// ─────────────────────────────────────────────────────────────────────────────
void drawTides() {
  // Clear tide area
  tft.fillRect(TIDE_X, 0, DIVIDER_X - TIDE_X, SCREEN_H, COL_BG);

  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);

  // Header
  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setFreeFont(NULL);
  tft.setTextSize(1);
  tft.drawString("TIDES", TIDE_X + 4, 4);

  if (tideCount == 0) {
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.drawString("Fetching...", TIDE_X + 4, 20);
    return;
  }

  // Each tide entry occupies ~55px vertically
  int rowH = (SCREEN_H - 10) / 4;

  for (int i = 0; i < tideCount; i++) {
    int y = 14 + i * rowH;
    uint16_t col = tides[i].isHigh ? COL_HIGH_TIDE : COL_LOW_TIDE;

    // Type label
    tft.setTextColor(col, COL_BG);
    tft.setTextSize(1);
    const char* label = tides[i].isHigh ? "HIGH" : "LOW ";
    tft.drawString(label, TIDE_X + 4, y);

    // Date
    tft.setTextSize(1);
    tft.drawString(tides[i].date, TIDE_X + 4, y + 11);

    // Time — slightly larger
    tft.setTextSize(2);
    tft.drawString(tides[i].time, TIDE_X + 2, y + 22);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw weather section (right third)
// ─────────────────────────────────────────────────────────────────────────────
void drawWeather() {
  int wx = DIVIDER_X + 3;
  int ww = SCREEN_W - wx - 2;

  // Clear weather area
  tft.fillRect(DIVIDER_X, 0, SCREEN_W - DIVIDER_X, SCREEN_H, COL_BG);

  // Divider line
  tft.drawFastVLine(DIVIDER_X, 0, SCREEN_H, COL_DIVIDER);

  // Header
  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setTextSize(1);
  tft.setTextDatum(TC_DATUM);
  int cx = wx + ww / 2;
  tft.drawString("WEATHER", cx, 4);
  tft.drawString("HHI", cx, 14);

  if (!weatherValid) {
    tft.setTextDatum(TC_DATUM);
    tft.drawString("...", cx, 80);
    return;
  }

  // Current temperature  (textSize 3 = large)
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", (int)roundf(currentTemp));

  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setTextSize(4);  // large
  tft.setTextDatum(TC_DATUM);
  tft.drawString(buf, cx, 50);

  // Degree F label
  tft.setTextSize(2);
  tft.drawString("o F", cx, 90);

  // Separator
  tft.drawFastHLine(wx, 115, ww, COL_DIVIDER);

  // "Feels like" label
  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setTextSize(1);
  tft.drawString("Feels Like", cx, 120);

  // Feels-like temperature  (textSize 2 ~= 80% of 3 rounded)
  snprintf(buf, sizeof(buf), "%d", (int)roundf(feelsLikeTemp));
  tft.setTextColor(COL_FEELS, COL_BG);  // bright red
  tft.setTextSize(3);  // slightly smaller than current temp
  tft.drawString(buf, cx, 140);

  tft.setTextSize(2);
  tft.drawString("o F", cx, 170);
}

// ─────────────────────────────────────────────────────────────────────────────
// Full redraw
// ─────────────────────────────────────────────────────────────────────────────
void drawAll() {
  tft.fillScreen(COL_BG);
  drawWave();
  drawTides();
  drawWeather();
}

// ─────────────────────────────────────────────────────────────────────────────
// Setup
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== TideDisplay starting ===");

  // Init display
  tft.init();
  // Rotation 3 = landscape, USB connector on the LEFT
  tft.setRotation(3);
  tft.fillScreen(COL_BG);
  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Connecting to WiFi...", SCREEN_W/2, SCREEN_H/2);

  // Connect WiFi
  WiFi.begin(SSID, PASSWORD);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 40) {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    tft.fillScreen(COL_BG);
    tft.setTextColor(COL_FEELS, COL_BG);
    tft.drawString("WiFi FAILED", SCREEN_W/2, SCREEN_H/2);
    Serial.println("\nWiFi connection failed!");
    return;
  }

  Serial.printf("\nWiFi connected: %s\n", WiFi.localIP().toString().c_str());

  tft.fillScreen(COL_BG);
  tft.setTextColor(COL_WHITE, COL_BG);
  tft.drawString("Fetching data...", SCREEN_W/2, SCREEN_H/2);

  // Initial data fetch
  fetchTides();
  fetchWeather();
  drawAll();
}

// ─────────────────────────────────────────────────────────────────────────────
// Loop
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();
  bool needRedraw = false;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    WiFi.reconnect();
    delay(5000);
    return;
  }

  if (now - lastTideFetch >= TIDE_REFRESH_MS || lastTideFetch == 0) {
    fetchTides();
    needRedraw = true;
  }

  if (now - lastWeatherFetch >= WEATHER_REFRESH_MS || lastWeatherFetch == 0) {
    fetchWeather();
    needRedraw = true;
  }

  if (needRedraw) {
    drawAll();
  }

  delay(30000);  // check every 30 seconds
}
