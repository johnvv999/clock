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
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <PNGdec.h>
#include "wave_img.h"
#include "weather_icons.h"
#include "basemap_img.h"

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

String rainForecastUrl = "";       // properties.forecastHourly from /points lookup
String rainForecastDailyUrl = "";  // properties.forecast (daily) from /points lookup

// ── Refresh intervals ─────────────────────────────────────────────────────────
const unsigned long TIDE_REFRESH_MS    = 6UL * 3600UL * 1000UL;
const unsigned long TIDE_RETRY_MS      = 10UL * 60UL * 1000UL;   // retry sooner after a failed tide fetch
const unsigned long WEATHER_REFRESH_MS = 10UL * 60UL * 1000UL;

// ── Display ───────────────────────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();

// ── Touch & sleep ─────────────────────────────────────────────────────────────
#define TOUCH_CS_PIN   33
#define TOUCH_IRQ_PIN  36
#define TOUCH_CLK_PIN  14   // shared with TFT HSPI bus
#define TOUCH_MOSI_PIN 13   // shared with TFT HSPI bus
#define TOUCH_MISO_PIN 12   // shared with TFT HSPI bus
#define BACKLIGHT_PIN  27
#define SLEEP_MS       600000UL

SPIClass touchSPI(HSPI);
XPT2046_Touchscreen touch(TOUCH_CS_PIN, TOUCH_IRQ_PIN);
bool screenOn = true;
unsigned long lastTouchMs = 0;
unsigned long lastSleepCountdownMs = 0;

// ── Double-tap detection ──────────────────────────────────────────────────────
bool touchWasDown = false;
unsigned long lastTapMs = 0;
const unsigned long DOUBLE_TAP_WINDOW_MS = 800;
const unsigned long MIN_TAP_GAP_MS       = 100;  // filters mechanical contact bounce
const int TOUCH_Z_THRESHOLD              = 150;  // lower = lighter touches register; raise if too sensitive

// ── Radar overlay mode ────────────────────────────────────────────────────────
bool radarMode = false;
unsigned long radarStartMs = 0;
const unsigned long RADAR_DURATION_MS = 60UL * 1000UL;

// NOAA's GeoServer WMS — lets us request an exact lat/lon bounding box rendered
// at our exact pixel size, at full native "Super Resolution" radar data, instead
// of cropping/zooming a fixed pre-made image. KCLX = Charleston, SC radar, which
// covers Hilton Head. No basemap/roads/labels come with this — just radar data —
// so we draw a simple crosshair marking your exact location for reference.
// Quality-controlled composite reflectivity — unlike the raw per-station "sr_bref"
// layer, this has ground clutter, sea clutter, and non-precipitation echoes filtered
// out by NOAA's MRMS system (important this close to the coast/marsh). It's a national
// mosaic, but WMS still lets us request just our small bbox at full native resolution.
const char* RADAR_WMS_HOST  = "https://opengeo.ncep.noaa.gov/geoserver/conus/ows";
const char* RADAR_WMS_LAYER = "conus_bref_qcd";

// (Basemap is now a static image baked into firmware — see basemap_img.h — rather
// than fetched from NOAA's reference map service, to avoid anti-aliasing speckle
// on this small a display.)

// Vertical half-extent of the view, in miles. Total view height = 2x this value;
// width is derived to match the screen's aspect ratio. Lower = more zoomed in.
const float RADAR_RADIUS_MI = 12.0f;

PNG png;

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
bool  tidesValid       = false;
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
  rainForecastUrl      = doc["properties"]["forecastHourly"].as<String>();
  rainForecastDailyUrl = doc["properties"]["forecast"].as<String>();
  Serial.printf("Resolved forecastHourly URL: %s\n", rainForecastUrl.c_str());
  Serial.printf("Resolved forecast URL: %s\n", rainForecastDailyUrl.c_str());
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

  String url = String("https://api.tidesandcurrents.noaa.gov/api/prod/datagetter") +
    "?begin_date=" + beginDate + "&end_date=" + endDate +
    "&station=" + NOAA_STATION +
    "&product=predictions&datum=MLLW&time_zone=lst_ldt"
    "&interval=hilo&units=english&application=tide_display&format=json";
  Serial.printf("Tide URL: %s\n", url.c_str());

  const int MAX_ATTEMPTS = 3;
  const unsigned long RETRY_DELAY_MS[MAX_ATTEMPTS] = {0, 5000, 15000};  // wait before attempts 2 and 3
  bool success = false;

  for (int attempt = 1; attempt <= MAX_ATTEMPTS && !success; attempt++) {
    if (RETRY_DELAY_MS[attempt - 1] > 0) {
      Serial.printf("Retrying tide fetch in %lus...\n", RETRY_DELAY_MS[attempt - 1] / 1000);
      delay(RETRY_DELAY_MS[attempt - 1]);
    }

    WiFiClientSecure secClient;
    secClient.setInsecure();
    HTTPClient http;
    http.begin(secClient, url);
    http.setTimeout(45000);  // NOAA can be slow under load — give it real time before giving up
    int code = http.GET();
    Serial.printf("Tide fetch attempt %d/%d: HTTP code %d\n", attempt, MAX_ATTEMPTS, code);

    if (code == 200) {
      String body = http.getString();
      http.end();

      DynamicJsonDocument doc(8192);
      DeserializationError err = deserializeJson(doc, body);
      if (err) {
        Serial.printf("Tide JSON parse error: %s\n", err.c_str());
        continue;  // treat as a failed attempt, try again
      }

      JsonArray predictions = doc["predictions"].as<JsonArray>();
      tideCount = 0;
      for (JsonObject p : predictions) {
        if (tideCount >= 4) break;
        parseNoaaTime(p["t"], tides[tideCount].time, tides[tideCount].date);
        tides[tideCount].isHigh = (p["type"].as<const char*>()[0] == 'H');
        tideCount++;
      }
      Serial.printf("Fetched %d tides\n", tideCount);
      success = true;
    } else {
      http.end();
    }
  }

  tidesValid    = success;
  lastTideFetch = millis();  // mark the attempt either way, so retries are spaced out, not spammed
  if (!success) {
    Serial.println("Tide fetch failed after retries — will retry in 10 minutes instead of waiting 6h.");
  }
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
      DynamicJsonDocument doc(6144);
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

          String obsDesc = doc["properties"]["textDescription"].as<String>();
          obsDesc.toLowerCase();
          Serial.printf("NWS observed: \"%s\"\n", obsDesc.c_str());
          if      (obsDesc.indexOf("thunder")  >= 0)                                      { weatherCode = lastWeatherCode = 95; }
          else if (obsDesc.indexOf("rain")     >= 0 || obsDesc.indexOf("shower") >= 0)   { weatherCode = lastWeatherCode = 61; }
          else if (obsDesc.indexOf("fog")      >= 0 || obsDesc.indexOf("mist")   >= 0)   { weatherCode = lastWeatherCode = 45; }
          else if (obsDesc.indexOf("cloud")    >= 0 || obsDesc.indexOf("overcast") >= 0) { weatherCode = lastWeatherCode = 2;  }
          else                                                                             { weatherCode = lastWeatherCode = 0;  }

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

  // --- NWS daily forecast: rain chance + conditions ---
  {
    if (rainForecastDailyUrl.length() == 0) {
      resolveGridpoint();
    }

    if (rainForecastDailyUrl.length() > 0) {
      WiFiClientSecure gpClient;
      gpClient.setInsecure();
      HTTPClient gpHttp;
      gpHttp.begin(gpClient, rainForecastDailyUrl);
      gpHttp.setTimeout(20000);
      gpHttp.addHeader("User-Agent", NWS_USER_AGENT);
      gpHttp.addHeader("Accept", "application/geo+json");
      int code = gpHttp.GET();

      if (code == 200) {
        DynamicJsonDocument doc(16384);
        DeserializationError err = deserializeJson(doc, gpHttp.getStream());
        if (!err) {
          rainChance = 0;
          String shortFc = "clear";
          String periodName = "";
          JsonArray periods = doc["properties"]["periods"].as<JsonArray>();
          int checked = 0;
          for (JsonObject period : periods) {
            if (checked++ >= 4) break;
            JsonVariant pop = period["probabilityOfPrecipitation"]["value"];
            int pct = pop.is<int>() ? pop.as<int>() : 0;
            if (pct > rainChance) {
              rainChance = pct;
              shortFc    = period["shortForecast"].as<String>();
              periodName = period["name"].as<String>();
            }
          }
          shortFc.toLowerCase();

          int newCode;
          if      (shortFc.indexOf("thunder")  >= 0)                                      newCode = 95;
          else if (shortFc.indexOf("rain")     >= 0 || shortFc.indexOf("shower") >= 0 ||
                   shortFc.indexOf("drizzle")  >= 0)                                       newCode = 61;
          else if (shortFc.indexOf("fog")      >= 0 || shortFc.indexOf("mist")   >= 0)    newCode = 45;
          else if (shortFc.indexOf("cloud")    >= 0 || shortFc.indexOf("overcast") >= 0)  newCode = 2;
          else                                                                              newCode = 0;

          if (newCode > weatherCode) { weatherCode = lastWeatherCode = newCode; }

          Serial.printf("NWS rain=%d%% period=\"%s\" forecast=\"%s\" code=%d\n",
                        rainChance, periodName.c_str(), shortFc.c_str(), weatherCode);
        } else {
          Serial.printf("NWS forecast JSON parse failed: %s\n", err.c_str());
        }
      } else {
        Serial.printf("NWS forecast HTTP error: %d\n", code);
        if (code == 301 || code == 404) rainForecastDailyUrl = "";
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
  tft.drawString("HHI", cx, 6);

  if (!weatherValid) {
    tft.drawString("...", cx, 80);
    return;
  }

  char buf[20];

  snprintf(buf, sizeof(buf), "%d F", (int)roundf(currentTemp));
  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setTextSize(3);
  tft.drawString(buf, cx, 20);

  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setTextSize(1);
  tft.drawString("Feels Like", cx, 52);

  snprintf(buf, sizeof(buf), "%d F", (int)roundf(feelsLikeTemp));
  tft.setTextColor(COL_FEELS, COL_BG);
  tft.setTextSize(2);
  tft.drawString(buf, cx, 64);

  int iconAreaY = 86;          // gives feels-like text (ends ~y=80) clear room above
  int iconAreaH = 98;
  int iconX = cx - WEATHER_ICON_W / 2;
  int iconY = iconAreaY + (iconAreaH - WEATHER_ICON_H) / 2;
  tft.fillRect(wx, iconAreaY, ww, iconAreaH, COL_BG);
  tft.setSwapBytes(false);
  tft.pushImage(iconX, iconY, WEATHER_ICON_W, WEATHER_ICON_H,
                (uint16_t*)getWeatherIcon(weatherCode), WEATHER_ICON_TRANSP);
  tft.setSwapBytes(true);

  int rainAreaY = iconAreaY + iconAreaH;
  tft.fillRect(wx, rainAreaY, ww, SCREEN_H - rainAreaY, COL_BG);

  tft.setTextColor(0x07FF, COL_BG);
  tft.setTextDatum(TC_DATUM);
  tft.setTextSize(2);
  tft.drawString("Rain", cx, rainAreaY + 4);

  snprintf(buf, sizeof(buf), "%d%%", rainChance);
  tft.setTextSize(3);
  tft.drawString(buf, cx, rainAreaY + 22);
}

// ── Radar overlay: static basemap image, then live radar data drawn on top ──
// Drawn ON TOP of the basemap — skip any pixel that came out as pure background
// color (i.e. "no radar echo here") so the basemap underneath stays visible.
int radarPngDraw(PNGDRAW *pDraw) {
  if (pDraw->y >= SCREEN_H) return 1;
  int lineW = min((int)pDraw->iWidth, SCREEN_W);
  uint16_t lineBuf[SCREEN_W];
  png.getLineAsRGB565(pDraw, lineBuf, PNG_RGB565_LITTLE_ENDIAN, 0x00000000);

  int x = 0;
  while (x < lineW) {
    if (lineBuf[x] == COL_BG) { x++; continue; }
    int runStart = x;
    while (x < lineW && lineBuf[x] != COL_BG) x++;
    tft.pushImage(runStart, pDraw->y, x - runStart, 1, &lineBuf[runStart]);
  }
  return 1;
}

// Downloads a PNG from `url` into a newly-malloc'd buffer. Handles both a known
// Content-Length and chunked/unknown-length responses. Caller must free() *outBuf
// on success. Returns true on success.
bool fetchPngToBuffer(const String &url, uint8_t **outBuf, int *outLen) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("User-Agent", NWS_USER_AGENT);
  http.useHTTP10(true);  // server closes after response — simplifies reading chunked data
  http.setTimeout(20000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("HTTP error %d: %s\n", code, url.c_str());
    http.end();
    return false;
  }

  int len = http.getSize();
  bool knownLength = (len > 0);
  if (knownLength && len > 250000) {
    Serial.printf("Response too large: %d\n", len);
    http.end();
    return false;
  }

  const int MAX_BYTES = 100000;  // safe ESP32 heap limit after WiFi stack
  int bufCap = knownLength ? min(len, MAX_BYTES) : MAX_BYTES;

  if ((int)ESP.getMaxAllocHeap() < bufCap + 10000) {
    Serial.printf("Not enough heap: largest=%u need=%d\n", ESP.getMaxAllocHeap(), bufCap);
    http.end();
    return false;
  }
  Serial.printf("Heap before malloc: free=%u, largest block=%u, requesting=%d\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap(), bufCap);

  uint8_t *buf = (uint8_t*)malloc(bufCap);
  if (!buf) {
    Serial.println("malloc failed");
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  int received = 0;
  unsigned long readStart = millis();
  while (millis() - readStart < 20000) {
    if (knownLength && received >= len) break;
    if (received >= bufCap) break;

    int avail = stream->available();
    if (avail > 0) {
      int toRead = min(avail, bufCap - received);
      received += stream->readBytes(buf + received, toRead);
    } else if (!http.connected()) {
      break;
    } else {
      delay(5);
    }
  }
  http.end();

  if (received == 0 || (knownLength && received != len)) {
    Serial.printf("Incomplete download (%d bytes received)\n", received);
    free(buf);
    return false;
  }
  Serial.printf("Bytes received: %d\n", received);

  *outBuf = buf;
  *outLen = received;
  return true;
}

// Decodes an already-downloaded PNG buffer via PNGdec using `drawCb`, then frees it.
bool decodePng(uint8_t *buf, int len, PNG_DRAW_CALLBACK *drawCb) {
  bool ok = false;
  int rc = png.openRAM(buf, len, drawCb);
  if (rc == PNG_SUCCESS) {
    tft.setSwapBytes(true);
    rc = png.decode(NULL, 0);
    png.close();
    Serial.printf("PNG decode result: %d\n", rc);
    ok = true;
  } else {
    Serial.printf("PNG open failed: %d\n", rc);
  }
  free(buf);
  return ok;
}

void showRadar() {
  // Build an exact lat/lon bounding box around Hilton Head, matched to the screen's
  // aspect ratio, so NOAA renders precisely the area we want at our exact resolution
  // — no client-side cropping or digital zoom needed.
  float latRad     = LOCATION_LAT * PI / 180.0f;
  float latHalfDeg = RADAR_RADIUS_MI / 69.0f;
  float lonHalfDeg = (RADAR_RADIUS_MI * ((float)SCREEN_W / SCREEN_H)) / (69.0f * cos(latRad));

  float lonMin = LOCATION_LON - lonHalfDeg;
  float lonMax = LOCATION_LON + lonHalfDeg;
  float latMin = LOCATION_LAT - latHalfDeg;
  float latMax = LOCATION_LAT + latHalfDeg;

  // Static basemap — baked into firmware, draws instantly, no network fetch needed.
  tft.setSwapBytes(true);
  tft.pushImage(0, 0, BASEMAP_W, BASEMAP_H, basemap_img);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setTextSize(2);
  tft.drawString("Loading radar...", SCREEN_W / 2, SCREEN_H / 2);

  char radarUrl[400];
  snprintf(radarUrl, sizeof(radarUrl),
    "%s?service=WMS&version=1.1.1&request=GetMap&layers=%s&styles="
    "&bbox=%.5f,%.5f,%.5f,%.5f&width=%d&height=%d&srs=EPSG:4326"
    "&format=image/png&transparent=true",
    RADAR_WMS_HOST, RADAR_WMS_LAYER, lonMin, latMin, lonMax, latMax, SCREEN_W, SCREEN_H);
  Serial.printf("Radar WMS URL: %s\n", radarUrl);

  uint8_t *radarBuf = nullptr;
  int radarLen = 0;
  bool downloaded = fetchPngToBuffer(radarUrl, &radarBuf, &radarLen);

  // Redraw the basemap now, before decoding — this clears the "Loading radar..."
  // text cleanly regardless of whether the radar layer ends up drawing anything
  // over that exact spot (e.g. on a clear day with no echoes nearby).
  tft.pushImage(0, 0, BASEMAP_W, BASEMAP_H, basemap_img);

  if (downloaded) {
    decodePng(radarBuf, radarLen, radarPngDraw);
  } else {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.setTextSize(2);
    tft.drawString("Radar unavailable", SCREEN_W / 2, SCREEN_H / 2);
  }

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setTextSize(1);
  tft.drawString("RAIN RADAR - HHI", 4, 4);

  char rangeBuf[24];
  snprintf(rangeBuf, sizeof(rangeBuf), "+/- %d mi", (int)RADAR_RADIUS_MI);
  tft.drawString(rangeBuf, 4, SCREEN_H - 12);
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
  touchSPI.begin(TOUCH_CLK_PIN, TOUCH_MISO_PIN, TOUCH_MOSI_PIN, TOUCH_CS_PIN);
  touch.begin(touchSPI);
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

  // --- Touch edge detection (polled every ~50ms for responsive double-tap) ---
  TS_Point p = touch.getPoint();
  bool touchedNow = (p.z > TOUCH_Z_THRESHOLD);
  if (touchedNow && !touchWasDown) {
    unsigned long gap = now - lastTapMs;

    if (gap < MIN_TAP_GAP_MS) {
      // Mechanical bounce of the same physical tap, not a new one — ignore.
      Serial.printf("Bounce ignored (gap=%lums)\n", gap);
    } else {
      Serial.printf("Tap: x=%d y=%d z=%d gap=%lums\n", p.x, p.y, p.z, gap);
      lastTouchMs = now;  // any real tap resets the sleep timer

      if (!screenOn) {
        digitalWrite(BACKLIGHT_PIN, HIGH);
        screenOn = true;
        if (!radarMode) drawAll();
        Serial.println("Screen ON (touch wake)");
      }

      if (gap <= DOUBLE_TAP_WINDOW_MS) {
        if (!radarMode) {
          Serial.println("Double-tap detected — showing radar");
          radarMode    = true;
          radarStartMs = now;
          showRadar();
        }
        lastTapMs = 0;  // consume the pair so a 3rd tap doesn't chain into another trigger
      } else {
        lastTapMs = now;
      }
    }
  }
  touchWasDown = touchedNow;

  // --- Radar mode timeout ---
  if (radarMode && (now - radarStartMs >= RADAR_DURATION_MS)) {
    radarMode = false;
    lastTouchMs = now;
    Serial.println("Radar timeout — returning to normal display");
    drawAll();
  }

  // --- Sleep timeout (paused while viewing radar) ---
  if (screenOn && !radarMode && (now - lastTouchMs >= SLEEP_MS)) {
    digitalWrite(BACKLIGHT_PIN, LOW);
    screenOn = false;
    Serial.println("Screen OFF (sleep)");
  }

  if (!screenOn) { delay(50); return; }

  if (!radarMode) {
    if (now - lastSleepCountdownMs >= 60000UL) {
      unsigned long elapsed     = now - lastTouchMs;
      unsigned long remainingMs = (elapsed >= SLEEP_MS) ? 0 : (SLEEP_MS - elapsed);
      unsigned long remainMin   = remainingMs / 60000UL;
      unsigned long remainSec   = (remainingMs / 1000UL) % 60UL;
      Serial.printf("Sleep in: %lum %02lus\n", remainMin, remainSec);
      lastSleepCountdownMs = now;
    }

    bool redraw = false;
    unsigned long tideInterval = tidesValid ? TIDE_REFRESH_MS : TIDE_RETRY_MS;
    if (now - lastTideFetch    >= tideInterval)       { fetchTides();   redraw = true; }
    if (now - lastWeatherFetch >= WEATHER_REFRESH_MS) { fetchWeather(); redraw = true; }
    if (redraw) drawAll();
  }

  delay(50);
}