/************************************************************
 * ESP32-E 3.2" Tide + Weather Display
 * WiFi: Dexter5G / madmax
 * Location: Hilton Head Island, SC (ZIP 29928)
 * Display: Inverted so USB port is on the LEFT (rotation 3)
 * Left 66%: Wave graphic + last 4 tide events
 * Right 33%: Current temp (white, large) + feels-like (red)
 ************************************************************/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

// ---- WiFi ----
const char* WIFI_SSID     = "Dexter5G";
const char* WIFI_PASSWORD = "madmax";

// ---- NOAA + Weather ----
const char* NOAA_STATION_ID = "8667999";  // Hilton Head
const char* WEATHER_API_KEY = "YOUR_OPENWEATHERMAP_API_KEY";

const float LAT_29928 = 32.204;
const float LON_29928 = -80.697;

// ---- Display Layout ----
const int SCREEN_W = 320;
const int SCREEN_H = 240;

const int LEFT_W   = (int)(SCREEN_W * 0.66);  // ~210 px
const int RIGHT_W  = SCREEN_W - LEFT_W;       // ~110 px

const int WAVE_W   = 80;                      // leftmost graphic width
const int TIDE_W   = LEFT_W - WAVE_W;

// ---- Colors ----
#define OLIVE_GREEN  tft.color565(128, 128, 0)
#define MEDIUM_BLUE  tft.color565(0, 102, 204)
#define BRIGHT_RED   tft.color565(255, 40, 40)

// ---- Update intervals ----
unsigned long lastWeatherUpdate = 0;
unsigned long lastTideUpdate    = 0;

const unsigned long WEATHER_UPDATE_MS = 5UL * 60UL * 1000UL;
const unsigned long TIDE_UPDATE_MS    = 15UL * 60UL * 1000UL;

// ---- Data ----
struct TideEvent {
  String timeStr;
  String typeStr;
};

TideEvent lastTides[4];

float currentTempF      = NAN;
float currentHumidity   = NAN;
float feelsLikeTempF    = NAN;

// ------------------------------------------------------------
// WiFi
// ------------------------------------------------------------
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected.");
}

// ------------------------------------------------------------
// Heat Index (Feels Like)
// ------------------------------------------------------------
float computeHeatIndexF(float T, float R) {
  if (isnan(T) || isnan(R)) return T;

  if (T < 80) return T;

  float HI = -42.379
             + 2.04901523 * T
             + 10.14333127 * R
             - 0.22475541 * T * R
             - 0.00683783 * T * T
             - 0.05481717 * R * R
             + 0.00122874 * T * T * R
             + 0.00085282 * T * R * R
             - 0.00000199 * T * T * R * R;

  return HI;
}

// ------------------------------------------------------------
// Weather Fetch
// ------------------------------------------------------------
void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String("https://api.openweathermap.org/data/2.5/weather?lat=") +
               LAT_29928 + "&lon=" + LON_29928 +
               "&units=imperial&appid=" + WEATHER_API_KEY;

  http.begin(url);
  int code = http.GET();

  if (code == 200) {
    StaticJsonDocument<2048> doc;
    deserializeJson(doc, http.getString());

    currentTempF    = doc["main"]["temp"].as<float>();
    currentHumidity = doc["main"]["humidity"].as<float>();
    feelsLikeTempF  = computeHeatIndexF(currentTempF, currentHumidity);
  }

  http.end();
}

// ------------------------------------------------------------
// Tide Fetch (NOAA)
// ------------------------------------------------------------
void fetchTides() {
  if (WiFi.status() != WL_CONNECTED) return;

  // For demo: static date. Replace with real date via NTP if desired.
  String dateStr = "20260628";

  String url = String("https://api.tidesandcurrents.noaa.gov/api/prod/datagetter") +
               "?product=predictions&application=arduino&datum=MLLW" +
               "&station=" + NOAA_STATION_ID +
               "&time_zone=lst_ldt&units=english&interval=hilo&format=json" +
               "&begin_date=" + dateStr + "&end_date=" + dateStr;

  HTTPClient http;
  http.begin(url);
  int code = http.GET();

  if (code == 200) {
    StaticJsonDocument<8192> doc;
    deserializeJson(doc, http.getString());

    JsonArray arr = doc["predictions"].as<JsonArray>();
    int n = arr.size();

    for (int i = 0; i < 4; i++) {
      int idx = n - 1 - i;

      if (idx < 0) {
        lastTides[i].timeStr = "--:--";
        lastTides[i].typeStr = "-";
        continue;
      }

      String t = arr[idx]["t"].as<String>();
      String type = arr[idx]["type"].as<String>();

      int hour = t.substring(11, 13).toInt();
      int min  = t.substring(14, 16).toInt();
      String ampm = "AM";

      if (hour == 0) hour = 12;
      else if (hour == 12) ampm = "PM";
      else if (hour > 12) { hour -= 12; ampm = "PM"; }

      char buf[16];
      snprintf(buf, sizeof(buf), "%02d:%02d %s", hour, min, ampm);

      lastTides[i].timeStr = buf;
      lastTides[i].typeStr = (type == "H") ? "High" : "Low";
    }
  }

  http.end();
}

// ------------------------------------------------------------
// Wave Graphic Placeholder
// ------------------------------------------------------------
void drawWaveGraphic() {
  tft.fillRect(0, 0, WAVE_W, SCREEN_H, TFT_NAVY);

  tft.fillCircle(WAVE_W/2, SCREEN_H-40, 50, TFT_BLUE);
  tft.fillCircle(WAVE_W/2+20, SCREEN_H-60, 40, TFT_CYAN);
  tft.fillCircle(WAVE_W/2+35, SCREEN_H-80, 30, TFT_WHITE);
}

// ------------------------------------------------------------
// Tide Section
// ------------------------------------------------------------
void drawTideSection() {
  int x0 = WAVE_W;
  int y0 = 10;
  int lineH = 32;

  tft.fillRect(x0, 0, TIDE_W, SCREEN_H, TFT_BLACK);

  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(x0 + 4, y0);
  tft.print("Last 4 Tides");

  for (int i = 0; i < 4; i++) {
    int y = y0 + 10 + (i + 1) * lineH;

    if (lastTides[i].typeStr == "High")
      tft.setTextColor(MEDIUM_BLUE, TFT_BLACK);
    else
      tft.setTextColor(OLIVE_GREEN, TFT_BLACK);

    tft.setCursor(x0 + 4, y);
    tft.printf("%s  %s", lastTides[i].typeStr.c_str(), lastTides[i].timeStr.c_str());
  }
}

// ------------------------------------------------------------
// Weather Section
// ------------------------------------------------------------
void drawWeatherSection() {
  int x0 = LEFT_W;

  tft.fillRect(x0, 0, RIGHT_W, SCREEN_H, TFT_BLACK);

  // Header
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(x0 + 4, 10);
  tft.print("Hilton Head");

  // Current temp (white, 20% larger)
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(3);
  tft.setCursor(x0 + 4, 50);
  if (!isnan(currentTempF)) tft.printf("%.1f F", currentTempF);
  else tft.print("--.- F");

  // Feels like (bright red)
  tft.setTextColor(BRIGHT_RED, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(x0 + 4, 95);
  tft.print("Feels like:");
  tft.setCursor(x0 + 4, 120);
  if (!isnan(feelsLikeTempF)) tft.printf("%.1f F", feelsLikeTempF);
  else tft.print("--.- F");

  // Humidity
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(x0 + 4, 155);
  tft.print("Humidity:");
  tft.setCursor(x0 + 4, 175);
  if (!isnan(currentHumidity)) tft.printf("%.0f %%", currentHumidity);
  else tft.print("-- %");
}

// ------------------------------------------------------------
// Setup
// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  connectWiFi();

  tft.init();
  tft.setRotation(3);   // USB-left landscape
  tft.fillScreen(TFT_BLACK);

  fetchWeather();
  fetchTides();

  drawWaveGraphic();
  drawTideSection();
  drawWeatherSection();
}

// ------------------------------------------------------------
// Loop
// ------------------------------------------------------------
void loop() {
  unsigned long now = millis();

  if (now - lastWeatherUpdate > WEATHER_UPDATE_MS) {
    lastWeatherUpdate = now;
    fetchWeather();
    drawWeatherSection();
  }

  if (now - lastTideUpdate > TIDE_UPDATE_MS) {
    lastTideUpdate = now;
    fetchTides();
    drawTideSection();
  }

  delay(200);
}
