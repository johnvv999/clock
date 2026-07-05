/*
 * TideDisplay.ino
 * ESP32 (ESP-32E) + 3.2" ST7789P3 TFT display (Hosyond)
 * Shows: Great Wave graphic | Next 4 tide times (H/L) | Current temp & feels-like
 * Location: Hilton Head Island, SC 29928  (NOAA Station 8665530)
 * Temperature + Feels-like: NWS KHXD (Hilton Head Airport) — real observed data
 * Rain chance + weather code: NWS gridpoint hourly forecast
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>
#include <WiFiClientSecure.h>
#include <XPT2046_Touchscreen.h>
#include "wave_img.h"
#include "weather_icons.h"

// ── WiFi ──────────────────────────────────────────────────────────────────────
const char* SSID     = "Dexter5G";
const char* PASSWORD = "maxmaddie";

// ── NTP ───────────────────────────────────────────────────────────────────────
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET = -18000;
const int   DST_OFFSET =  3600;

// ── NOAA Station: Hilton Head Island SC ───────────────────────────────────────
const char* NOAA_STATION = "8665530";

// ── NWS observed data: Hilton Head Airport (KHXD) ────────────────────────────
const char* NWS_URL = "https://api.weather.gov/stations/KHXD/observations/latest";

// ── NWS gridpoint: rain chance + weather code (resolved once at boot) ────────
const float LOCATION_LAT     = 32.2163;
const float LOCATION_LON     = -80.7526;
const char* NWS_USER_AGENT   = "(TideDisplay, johnvv999@example.com)";

String rainForecastUrl = "";  // properties.forecastHourly from /points lookup

// ── Refresh intervals ─────────────────────────────────────────────────────────
const unsigned long TIDE_REFRESH_MS    = 6UL * 3600UL * 1000UL;
const unsigned long WEATHER_REFRESH_MS = 10UL * 60UL * 1000UL;

// ── Display ───────────────────────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();

// ── Touch & sleep ─────────────────────────────────────────────────────────────
#define TOUCH_CS_PIN  33
#define TOUCH_IRQ_PIN 36
#define BACKLIGHT_PIN 27
#define SLEEP_MS      600000UL

XPT2046_Touchscreen touch(TOUCH_CS_PIN, TOUCH_IRQ_PIN);
bool screenOn = true;
unsigned long lastTouchMs = 0;

const int DIVIDER_X = 205;
const int WAVE_X    = 0;
const int TIDE_X    = 110;
const int SCREEN_W  = 320;
const int SCREEN_H  = 240;

// ── Colours ───────────────────────────────────────────────────────────────────
#define COL_BG        0x0000
#define COL_WHITE     0xFFFF
#define COL_LOW_TIDE  0x6CC0
#define COL_HIGH_TIDE 0x34BF
#define COL_FEELS     0xF800
#define COL_DIVIDER   0x2945

// ── Data ──────────────────────────────────────────────────────────────────────
struct TideEvent {
  char time[12];
  char date[10];
  bool isHigh;
};

TideEvent tides[4];
int   tideCount       = 0;
float currentTemp     = 0.0f;
float feelsLikeTemp   = 0.0f;
int   weatherCode     = 0;
int   lastWeatherCode = 0;
int   rainChance      = 0;
bool  weatherValid    = false;

unsigned long lastTideFetch    = 0;
unsigned long lastWeatherFetch = 0;

// ── Heat index (NWS Rothfusz) — valid for T >= 80F, RH >= 40% ────────────────
float heatIndex(float T, float RH) {
  return -42.379
       +  2.04901523  * T
       + 10.14333127  * RH
       -  0.22475541  * T  * RH
       -  0.00683783  * T  * T
       -  0.05481717  * RH * RH
       +  0.00122874  * T  * T  * RH
       +  0.00085282  * T  * RH * RH
       -  0.00000199  * T  * T  * RH * RH;
}

// ── Wind chill (NWS formula) — valid for T <= 50F, wind >= 3 mph ─────────────
float windChill(float T, float V) {
  return 35.74
       +  0.6215  * T
       - 35.75    * pow(V, 0.16f)
       +  0.4275  * T * pow(V, 0.16f);
}

// ── Feels-like: picks the right formula for conditions ────────────────────────
float calcFeelsLike(float tempF, float rh, float windMph) {
  if (tempF >= 80.0f && rh >= 40.0f) {
    return heatIndex(tempF, rh);
  } else if (tempF <= 50.0f && windMph >= 3.0f) {
    return windChill(tempF, windMph);
  } else {
    return tempF;   // comfortable range — actual temp is feels-like
  }
}

// ── Parse NOAA time ───────────────────────────────────────────────────────────
void parseNoaaTime(const char* iso, char* timeOut, char* dateOut) {
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






// ── Resolve NWS gridpoint forecastHourly URL (call once; cache result) ───────
bool resolveGridpoint() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  char url[80];
  snprintf(url, sizeof(url), "https://api.weather.gov/points/%.4f,%.4f", LOCATION_LAT, LOCATION_LON);
  https.begin(client, url);
  https.addHeader("User-Agent", NWS_USER_AGENT);
  https.setTimeout(20000);
  int code = https.GET();
  if (code != 200) {
    Serial.printf("Gridpoint lookup failed: %d\n", code);
    https.end();
    return false;
  }
  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, https.getStream());
  https.end();
  if (err) {
    Serial.printf("Gridpoint JSON parse error: %s\n", err.c_str());
    return false;
  }
  rainForecastUrl = doc["properties"]["forecastHourly"].as<String>();
  Serial.printf("Resolved forecastHourly URL: %s\n", rainForecastUrl.c_str());
  return rainForecastUrl.length() > 0;
}

// ── Fetch tides ───────────────────────────────────────────────────────────────
void fetchTides() {
  time_t now = time(nullptr);
  struct tm ti;
  localtime_r(&now, &ti);
  char beginDate[10], endDate[10];
  strftime(beginDate, sizeof(beginDate), "%Y%m%d", &ti);
  time_t future = now + 2 * 86400UL;
  struct tm ti2;
  localtime_r(&future, &ti2);
  strftime(endDate, sizeof(endDate), "%Y%m%d", &ti2);

  Serial.printf("Tide fetch: begin=%s end=%s\n", beginDate, endDate);

  WiFiClientSecure secClient;
  secClient.setInsecure();
  HTTPClient http;
  String url = String("https://api.tidesandcurrents.noaa.gov/api/prod/datagetter") +
    "?begin_date=" + beginDate + "&end_date=" + endDate +
    "&station=" + NOAA_STATION +
    "&product=predictions&datum=MLLW&time_zone=lst_ldt"
    "&interval=hilo&units=english&application=tide_display&format=json";
  Serial.printf("Tide URL: %s\n", url.c_str());
  http.begin(secClient, url);
  http.setTimeout(20000);
  int code = http.GET();
  Serial.printf("Tide HTTP code: %d\n", code);
  if (code != 200) { http.end(); return; }
  String body = http.getString();
  Serial.printf("Tide body (%d bytes): %.200s\n", body.length(), body.c_str());
  http.end();

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("Tide JSON parse error: %s\n", err.c_str());
    return;
  }

  JsonArray predictions = doc["predictions"].as<JsonArray>();
  Serial.printf("Predictions array size: %d\n", predictions.size());
  tideCount = 0;
  for (JsonObject p : predictions) {
    if (tideCount >= 4) break;
    parseNoaaTime(p["t"], tides[tideCount].time, tides[tideCount].date);
    tides[tideCount].isHigh = (p["type"].as<const char*>()[0] == 'H');
    tideCount++;
  }
  Serial.printf("Fetched %d tides\n", tideCount);
  lastTideFetch = millis();
}








// ── Fetch weather ─────────────────────────────────────────────────────────────
void fetchWeather() {

  // --- NWS: temperature, humidity, wind — all from real KHXD station ---
  {
    WiFiClientSecure nwsClient;
    nwsClient.setInsecure();
    HTTPClient nwsHttp;
    nwsHttp.begin(nwsClient, NWS_URL);
    nwsHttp.setTimeout(20000);
    nwsHttp.addHeader("User-Agent", "HiltonHeadTideDisplay/1.0");
    nwsHttp.addHeader("Accept",     "application/geo+json");
    int code = nwsHttp.GET();
    if (code == 200) {
      String body = nwsHttp.getString();
      DynamicJsonDocument doc(4096);
      if (!deserializeJson(doc, body)) {
        JsonVariant tempVal = doc["properties"]["temperature"]["value"];
        JsonVariant rhVal   = doc["properties"]["relativeHumidity"]["value"];
        JsonVariant wndVal  = doc["properties"]["windSpeed"]["value"]; // km/h

        if (!tempVal.isNull()) {
          float tempC   = tempVal.as<float>();
          currentTemp   = tempC * 9.0f / 5.0f + 32.0f;

          float rh      = rhVal.isNull()  ? 50.0f : rhVal.as<float>();
          float windKph = wndVal.isNull() ?  0.0f : wndVal.as<float>();
          float windMph = windKph * 0.621371f;

          feelsLikeTemp = calcFeelsLike(currentTemp, rh, windMph);

          Serial.printf("NWS temp: %.1fF  RH: %.0f%%  Wind: %.1fmph  Feels: %.1fF\n",
                        currentTemp, rh, windMph, feelsLikeTemp);
        } else {
          Serial.println("NWS temp null — keeping last value");
        }
      } else {
        Serial.println("NWS JSON parse failed");
      }
    } else {
      Serial.printf("NWS HTTP error: %d\n", code);
    }
    nwsHttp.end();
  }

  // --- NWS gridpoint hourly forecast: rain chance + conditions ---
  {
    if (rainForecastUrl.length() == 0) {
      resolveGridpoint();
    }

    if (rainForecastUrl.length() > 0) {
      WiFiClientSecure gpClient;
      gpClient.setInsecure();
      HTTPClient gpHttp;
      gpHttp.begin(gpClient, rainForecastUrl);
      gpHttp.setTimeout(20000);
      gpHttp.addHeader("User-Agent", NWS_USER_AGENT);
      gpHttp.addHeader("Accept", "application/geo+json");
      int code = gpHttp.GET();

      if (code == 200) {
        // Only keep the fields we need from periods[0] — keeps the doc small
        StaticJsonDocument<128> filter;
        filter["properties"]["periods"][0]["probabilityOfPrecipitation"] = true;
        filter["properties"]["periods"][0]["shortForecast"] = true;

        DynamicJsonDocument doc(16384);
        DeserializationError err = deserializeJson(doc, gpHttp.getStream(),
                                                     DeserializationOption::Filter(filter));
        if (!err) {
          JsonVariant pop = doc["properties"]["periods"][0]["probabilityOfPrecipitation"]["value"];
          rainChance = pop.is<int>() ? pop.as<int>() : 0;

          String shortFc = doc["properties"]["periods"][0]["shortForecast"].as<String>();
          shortFc.toLowerCase();

          int newCode;
          if (shortFc.indexOf("thunder") >= 0) {
            newCode = 95;
          } else if (shortFc.indexOf("rain") >= 0 || shortFc.indexOf("shower") >= 0 ||
                     shortFc.indexOf("drizzle") >= 0) {
            newCode = 61;
          } else if (shortFc.indexOf("fog") >= 0 || shortFc.indexOf("mist") >= 0 ||
                     shortFc.indexOf("haze") >= 0) {
            newCode = 45;
          } else if (shortFc.indexOf("cloud") >= 0 || shortFc.indexOf("overcast") >= 0) {
            newCode = 2;
          } else {
            newCode = 0;  // clear / sunny / mostly sunny
          }

          if (newCode <= 3 || rainChance > 50) {
            weatherCode     = newCode;
            lastWeatherCode = newCode;
          } else {
            weatherCode = lastWeatherCode;
          }

          Serial.printf("NWS rain=%d%%  forecast=\"%s\"  code=%d\n",
                        rainChance, shortFc.c_str(), weatherCode);
        } else {
          Serial.printf("NWS gridpoint JSON parse failed: %s\n", err.c_str());
        }
      } else {
        Serial.printf("NWS gridpoint HTTP error: %d\n", code);
        if (code == 301 || code == 404) {
          // grid mapping may have shifted — force a re-lookup next cycle
          rainForecastUrl = "";
        }
      }
      gpHttp.end();
    }

    if (currentTemp != 0.0f) weatherValid = true;
    lastWeatherFetch = millis();
  }

  weatherValid     = true;
  lastWeatherFetch = millis();
  Serial.printf("Weather final: %.1fF feels %.1fF code=%d rain=%d%%\n",
                currentTemp, feelsLikeTemp, weatherCode, rainChance);
}

// ── Draw wave ─────────────────────────────────────────────────────────────────


void drawWave() {
  int y0 = (SCREEN_H - WAVE_H) / 2;
  if (y0 < 0) y0 = 0;
  tft.setSwapBytes(true);   // natural data, swapped for display + matches transp check
  tft.pushImage(WAVE_X, y0, WAVE_W, WAVE_H, (uint16_t*)wave_img, WAVE_TRANSP);
  tft.setSwapBytes(true);
}

// ── Draw tides ────────────────────────────────────────────────────────────────
void drawTides() {
  tft.fillRect(TIDE_X, 0, DIVIDER_X - TIDE_X, SCREEN_H, COL_BG);
  tft.setTextDatum(TL_DATUM);
  if (tideCount == 0) {
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.setTextSize(1);
    tft.drawString("Fetching...", TIDE_X + 4, 16);
    return;
  }
  int rowH = SCREEN_H / 4;
  for (int i = 0; i < tideCount; i++) {
    int y = i * rowH + 4;
    uint16_t col = tides[i].isHigh ? COL_HIGH_TIDE : COL_LOW_TIDE;
    tft.setTextColor(col, COL_BG);
    tft.setTextSize(1);
    tft.drawString(tides[i].isHigh ? "HIGH" : "LOW", TIDE_X + 4, y);
    tft.drawString(tides[i].date, TIDE_X + 4, y + 12);
    tft.setTextSize(2);
    tft.drawString(tides[i].time, TIDE_X + 2, y + 24);
    
  }
}

// ── Weather icon ──────────────────────────────────────────────────────────────
const uint16_t* getWeatherIcon(int code) {
  if (code == 45 || code == 48)   return weather_fog;
  if (code == 0 || code == 1)     return weather_sun;
  if (code == 2 || code == 3)     return weather_overcast;
  if (code >= 51 && code <= 82)   return weather_rain;
  if (code >= 95)                 return weather_thunder;
  return weather_overcast;
}

// ── Draw weather ──────────────────────────────────────────────────────────────
void drawWeather() {
  tft.fillRect(DIVIDER_X, 0, SCREEN_W - DIVIDER_X, SCREEN_H, COL_BG);
  tft.drawFastVLine(DIVIDER_X, 0, SCREEN_H, COL_DIVIDER);

  int wx = DIVIDER_X + 3;
  int ww = SCREEN_W - wx - 2;
  int cx = wx + ww / 2;

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setTextSize(1);
  tft.drawString("WEATHER", cx, 4);
  tft.drawString("HHI", cx, 14);

  if (!weatherValid) {
    tft.drawString("...", cx, 80);
    return;
  }

  char buf[20];

  snprintf(buf, sizeof(buf), "%d F", (int)roundf(currentTemp));
  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setTextSize(3);
  tft.drawString(buf, cx, 30);

 // tft.drawFastHLine(wx, 60, ww, COL_DIVIDER);

  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setTextSize(1);
  tft.drawString("Feels Like", cx, 65);

  snprintf(buf, sizeof(buf), "%d F", (int)roundf(feelsLikeTemp));
  tft.setTextColor(COL_FEELS, COL_BG);
  tft.setTextSize(2);
  tft.drawString(buf, cx, 78);

  //tft.drawFastHLine(wx, 100, ww, COL_DIVIDER);

  int iconX = cx - WEATHER_ICON_W / 2;
  int iconY = 102 + (126 - WEATHER_ICON_H) / 2;
  tft.fillRect(wx, 102, ww, 126, COL_BG);
  tft.setSwapBytes(false);
  tft.pushImage(iconX, iconY, WEATHER_ICON_W, WEATHER_ICON_H,
                (uint16_t*)getWeatherIcon(weatherCode), WEATHER_ICON_TRANSP);
  tft.setSwapBytes(true);

  snprintf(buf, sizeof(buf), "Rain: %d%%", rainChance);
  tft.setTextColor(0x07FF, COL_BG);
  tft.setTextSize(1);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(buf, cx, 228);
}

// ── Full redraw ───────────────────────────────────────────────────────────────
void drawAll() {
  tft.fillScreen(COL_BG);
  drawWave();
  drawTides();
  drawWeather();
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== TideDisplay starting ===");

  pinMode(27, OUTPUT);
  digitalWrite(27, HIGH);
  delay(100);

  tft.init();
  tft.setRotation(3);
  tft.setSwapBytes(true);
  touch.begin();
  touch.setRotation(3);
  lastTouchMs = millis();

  tft.fillScreen(TFT_RED);   delay(500);
  tft.fillScreen(TFT_GREEN); delay(500);
  tft.fillScreen(TFT_BLUE);  delay(500);
  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Connecting WiFi...", SCREEN_W/2, SCREEN_H/2);

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
    Serial.println("\nWiFi FAILED");
    return;
  }
  Serial.printf("\nWiFi OK: %s\n", WiFi.localIP().toString().c_str());

  tft.fillScreen(COL_BG);
  tft.setTextColor(COL_WHITE, COL_BG);
  tft.drawString("Syncing time...", SCREEN_W/2, SCREEN_H/2);
  configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER);

  time_t t = 0;
  for (int i = 0; i < 20 && t < 100000; i++) {
    delay(500);
    t = time(nullptr);
    Serial.printf("NTP t=%lu\n", t);
  }
  Serial.printf("Time synced: %s", ctime(&t));

  tft.fillScreen(COL_BG);
  tft.drawString("Fetching data...", SCREEN_W/2, SCREEN_H/2);

  resolveGridpoint();
  fetchTides();
  fetchWeather();
  drawAll();
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(5000);
    return;
  }

  if (touch.tirqTouched() && touch.touched()) {
    lastTouchMs = now;
    if (!screenOn) {
      digitalWrite(BACKLIGHT_PIN, HIGH);
      screenOn = true;
      drawAll();
      Serial.println("Screen ON (touch wake)");
    }
  }

  if (screenOn && (now - lastTouchMs >= SLEEP_MS)) {
    digitalWrite(BACKLIGHT_PIN, LOW);
    screenOn = false;
    Serial.println("Screen OFF (sleep)");
  }

  if (!screenOn) { delay(500); return; }

  bool redraw = false;
  if (now - lastTideFetch    >= TIDE_REFRESH_MS)    { fetchTides();   redraw = true; }
  if (now - lastWeatherFetch >= WEATHER_REFRESH_MS) { fetchWeather(); redraw = true; }
  if (redraw) drawAll();

  delay(30000);
}