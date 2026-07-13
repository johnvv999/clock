/*
 * TideDisplay.ino
 * ESP32 (ESP-32E) + 3.2" ST7789P3 TFT display (Hosyond)
 * Shows: Great Wave graphic | Next 4 tide times (H/L) | Current temp & feels-like
 * Location: Hilton Head Island, SC 29928  (NOAA Station 8665530)
 * Temperature + Feels-like: NWS KHXD (Hilton Head Airport) — real observed data
 * Rain chance + weather code: NWS gridpoint hourly forecast
 *
 * Screens: home -> (double-tap) -> radar -> (tap) -> today's forecast -> (tap) -> home
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
// PNGdec's internal per-line pixel buffer defaults to exactly enough for a
// 320px-wide image at 32bpp (see PNGdec 1.1.5 release notes). Our radar PNGs
// are 320px wide with an alpha channel (32bpp) — exactly that boundary. This
// lines up with the corruption we've been seeing: decode() can report success
// while pixel data mid-image is wrong, and other times it comes up a couple
// rows short. Override with headroom so we're safely clear of the edge case.
#define PNG_MAX_BUFFERED_PIXELS ((384 * 4 + 1) * 2)
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

String rainForecastUrl      = "";  // properties.forecastHourly from /points lookup
String rainForecastDailyUrl = "";  // properties.forecast (daily) from /points lookup — today's high/low

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

// ── Forecast screen mode ──────────────────────────────────────────────────────
// Tap sequence: home (double-tap) → radar (tap) → forecast (tap) → home.
bool forecastMode = false;
unsigned long forecastStartMs = 0;
const unsigned long FORECAST_DURATION_MS = 60UL * 1000UL;

// Only count a period as "rain starting" if the forecast is reasonably confident
// (chance of precipitation above this threshold) — filters out low-confidence
// "slight chance" mentions from triggering the callout on the forecast screen.
const int RAIN_START_THRESHOLD_PCT = 50;

// NOAA's GeoServer WMS — lets us request an exact lat/lon bounding box rendered
// at our exact pixel size, at full native "Super Resolution" radar data, instead
// of cropping/zooming a fixed pre-made image. KCLX = Charleston, SC radar, which
// covers Hilton Head. No basemap/roads/labels come with this — just radar data —
// so we draw a simple crosshair marking your exact location for reference.
// Trying Iowa State's Iowa Environmental Mesonet (IEM) instead of NOAA's
// GeoServer: same WMS GetMap protocol (bbox/width/height/format all carry
// over unchanged), same unprojected lat/lon (EPSG:4326), updated every 5
// minutes. Switching in case it's a more reliable/robust server than NOAA's
// instance — worth testing empirically.
//
// Tradeoff: this is IEM's raw NEXRAD base reflectivity composite (N0Q), not
// a quality-controlled product. NOAA's old conus_bref_qcd layer (commented
// out below) had ground/sea clutter and non-precip echoes filtered out by
// MRMS, which matters this close to the coast/marsh — this source may show
// more clutter/noise than we're used to. If that's a problem in practice,
// revert to the NOAA line below.
const char* RADAR_WMS_HOST  = "https://mesonet.agron.iastate.edu/cgi-bin/wms/nexrad/n0q.cgi";
const char* RADAR_WMS_LAYER = "nexrad-n0q";
// const char* RADAR_WMS_HOST  = "https://opengeo.ncep.noaa.gov/geoserver/conus/ows";
// const char* RADAR_WMS_LAYER = "conus_bref_qcd";

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
int   rainChance      = 0;
bool  weatherValid    = false;

// ── Today's forecast screen data ─────────────────────────────────────────────
struct ForecastHour {
  char time[10];   // e.g. "2 PM" / "2:30 PM"
  int  pop;        // rain chance, %
  bool isRain;      // shortForecast for this hour mentions rain/showers/thunder
};
const int MAX_FORECAST_HOURS = 8;
ForecastHour todayForecast[MAX_FORECAST_HOURS];
int  todayForecastCount = 0;
bool rainExpectedToday  = false;
char rainStartTime[10]  = "";
int  todayMaxRainChance = 0;
bool forecastDataValid  = false;
int  todayHighTemp      = 0;
int  todayLowTemp       = 0;
bool todayHighValid     = false;
bool todayLowValid      = false;

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

// ── Parse NWS ISO8601 time (e.g. "2026-07-10T14:00:00-04:00") ───────────────
// dateOut gets "YYYYMMDD" (for same-day comparisons), timeOut gets a compact
// 12-hour clock string ("2 PM" / "2:30 PM"). Either pointer may be NULL.
void parseNwsIsoTime(const char* iso, char* dateOut, char* timeOut) {
  int yr, mo, dy, hr, mn;
  sscanf(iso, "%d-%d-%dT%d:%d", &yr, &mo, &dy, &hr, &mn);
  if (dateOut) snprintf(dateOut, 9, "%04d%02d%02d", yr, mo, dy);
  if (timeOut) {
    const char* ampm = (hr >= 12) ? "PM" : "AM";
    int hr12 = hr % 12;
    if (hr12 == 0) hr12 = 12;
    if (mn == 0) snprintf(timeOut, 10, "%d %s", hr12, ampm);
    else         snprintf(timeOut, 10, "%d:%02d %s", hr12, mn, ampm);
  }
}






// ── Patient stream reader for ArduinoJson ────────────────────────────────────
// Stream::read() is non-blocking — it returns -1 the instant a byte isn't
// available *yet*, not just at true end-of-stream. ArduinoJson's default Stream
// handling takes that at face value, so on a large response arriving over WiFi/
// TLS in bursts, a normal brief gap between packets can look identical to EOF,
// producing IncompleteInput or (worse, when combined with a Filter) a silently
// empty result. This wraps the stream the same patient way fetchPngToBuffer()
// already reads the radar PNG: keep waiting on available()/connected() instead
// of trusting a single non-blocking read().
class PatientStreamReader {
  public:
    PatientStreamReader(Stream &s, HTTPClient &h) : _stream(s), _http(h) {}
    int read() {
      unsigned long start = millis();
      while (millis() - start < 10000) {
        if (_stream.available() > 0) return _stream.read();
        if (!_http.connected()) return _stream.available() > 0 ? _stream.read() : -1;
        delay(2);
      }
      return -1;  // gave up waiting — genuinely stalled
    }
    size_t readBytes(char *buffer, size_t length) {
      size_t count = 0;
      while (count < length) {
        int c = read();
        if (c < 0) break;
        buffer[count++] = (char)c;
      }
      return count;
    }
  private:
    Stream &_stream;
    HTTPClient &_http;
};

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
  Serial.printf("Resolved forecast (daily) URL: %s\n", rainForecastDailyUrl.c_str());
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
  time_t nowT = time(nullptr);
  struct tm ti;
  localtime_r(&nowT, &ti);
  char todayDate[9];
  strftime(todayDate, sizeof(todayDate), "%Y%m%d", &ti);

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
          if      (obsDesc.indexOf("thunder")  >= 0)                                      { weatherCode = 95; }
          else if (obsDesc.indexOf("rain")     >= 0 || obsDesc.indexOf("shower") >= 0)   { weatherCode = 61; }
          else if (obsDesc.indexOf("fog")      >= 0 || obsDesc.indexOf("mist")   >= 0)   { weatherCode = 45; }
          else if (obsDesc.indexOf("cloud")    >= 0 || obsDesc.indexOf("overcast") >= 0) { weatherCode = 2;  }
          else                                                                             { weatherCode = 0;  }

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

  // --- NWS hourly forecast: rain chance + conditions ---
  {
    if (rainForecastUrl.length() == 0) {
      resolveGridpoint();
    }

    if (rainForecastUrl.length() > 0) {
      WiFiClientSecure gpClient;
      gpClient.setInsecure();
      gpClient.setTimeout(20000);
      HTTPClient gpHttp;
      gpHttp.begin(gpClient, rainForecastUrl);
      gpHttp.setTimeout(20000);
      // Content-Length came back -1 (chunked transfer-encoding, confirmed by log).
      // Same fix already used for the radar PNG fetch: HTTP/1.0 makes the server
      // send a real length (or just close the connection) instead of interleaving
      // chunk-size markers into the body. Those markers landing inside the stream
      // that ArduinoJson's filter reads byte-by-byte would explain periods coming
      // back empty with no parse error — the filter's skip logic doesn't validate
      // JSON structure strictly, so stray non-JSON bytes can throw off its brace
      // tracking without ever raising a hard error.
      gpHttp.useHTTP10(true);
      gpHttp.addHeader("User-Agent", NWS_USER_AGENT);
      gpHttp.addHeader("Accept", "application/geo+json");
      int code = gpHttp.GET();

      if (code == 200) {
        Serial.printf("Hourly response Content-Length: %d\n", gpHttp.getSize());
        // Hourly forecast has ~150 periods (vs. ~14 for the daily endpoint) — filter
        // down to just the fields we use so the parsed doc stays small on the ESP32.
        // (Sized well above the ~192B this structure theoretically needs — a filter
        // sized to the exact minimum can silently truncate during construction and
        // end up filtering out everything, which is what was causing "hours=0".)
        StaticJsonDocument<512> filter;
        filter["properties"]["periods"][0]["startTime"] = true;
        filter["properties"]["periods"][0]["shortForecast"] = true;
        // Whole subtree, not just ["value"] — nested field-selection inside an
        // already-filtered array element is the one structural difference between
        // this (broken, periods=0) filter and the daily one (which works and only
        // filters flat/scalar fields). Including the whole object is a superset
        // (keeps the small "unitCode" string too) but sidesteps whatever's wrong
        // with the doubly-nested selector.
        filter["properties"]["periods"][0]["probabilityOfPrecipitation"] = true;

        // Bumped up from the original 16KB now that startTime is included and we
        // walk up to 24 periods instead of 4 — still comfortably freed right after
        // this block since `doc` is scoped to it.
        DynamicJsonDocument doc(24576);
        PatientStreamReader reader(gpHttp.getStream(), gpHttp);
        DeserializationError err = deserializeJson(doc, reader,
                                                     DeserializationOption::Filter(filter));
        if (!err) {
          rainChance = 0; String shortFc = "clear"; String bestPeriodTime = "";
          todayForecastCount = 0;
          rainExpectedToday  = false;
          rainStartTime[0]   = '\0';
          todayMaxRainChance = 0;

          JsonArray periods = doc["properties"]["periods"].as<JsonArray>();
          Serial.printf("Hourly periods parsed: %d (todayDate=%s, overflowed=%d, hasProperties=%d)\n",
                        periods.size(), todayDate, doc.overflowed(), !doc["properties"].isNull());
          int idx = 0;
          for (JsonObject period : periods) {
            if (idx >= 24) break;  // next 24 hourly periods is plenty to cover "today"

            const char* startIso = period["startTime"] | "";
            char pDate[9], pTime[10];
            parseNwsIsoTime(startIso, pDate, pTime);

            JsonVariant pop = period["probabilityOfPrecipitation"]["value"];
            int pct = pop.is<int>() ? pop.as<int>() : 0;
            String periodFc = period["shortForecast"].as<String>();
            String periodFcLower = periodFc;
            periodFcLower.toLowerCase();
            bool periodIsRain = periodFcLower.indexOf("rain")    >= 0 ||
                                 periodFcLower.indexOf("shower")  >= 0 ||
                                 periodFcLower.indexOf("drizzle") >= 0 ||
                                 periodFcLower.indexOf("thunder") >= 0;

            // Near-term (next 4 hours): drives the compact rain% + icon on the home screen.
            if (idx < 4 && pct > rainChance) {
              rainChance     = pct;
              shortFc        = periodFc;
              bestPeriodTime = String(pTime);
            }

            // Today's forecast screen: every hour still on today's calendar date.
            if (strcmp(pDate, todayDate) == 0) {
              if (pct > todayMaxRainChance) todayMaxRainChance = pct;
              if (pct > RAIN_START_THRESHOLD_PCT && !rainExpectedToday) {
                rainExpectedToday = true;
                strncpy(rainStartTime, pTime, sizeof(rainStartTime) - 1);
                rainStartTime[sizeof(rainStartTime) - 1] = '\0';
              }
              if (todayForecastCount < MAX_FORECAST_HOURS) {
                strncpy(todayForecast[todayForecastCount].time, pTime,
                        sizeof(todayForecast[todayForecastCount].time) - 1);
                todayForecast[todayForecastCount].time[sizeof(todayForecast[todayForecastCount].time) - 1] = '\0';
                todayForecast[todayForecastCount].pop    = pct;
                todayForecast[todayForecastCount].isRain = periodIsRain;
                todayForecastCount++;
              }
            }
            idx++;
          }
          forecastDataValid = true;

          shortFc.toLowerCase();
          int newCode;
          if      (shortFc.indexOf("thunder")  >= 0)                                      newCode = 95;
          else if (shortFc.indexOf("rain")     >= 0 || shortFc.indexOf("shower") >= 0 ||
                   shortFc.indexOf("drizzle")  >= 0)                                       newCode = 61;
          else if (shortFc.indexOf("fog")      >= 0 || shortFc.indexOf("mist")   >= 0)    newCode = 45;
          else if (shortFc.indexOf("cloud")    >= 0 || shortFc.indexOf("overcast") >= 0)  newCode = 2;
          else                                                                              newCode = 0;
          if (newCode > weatherCode) { weatherCode = newCode; }
          Serial.printf("NWS rain=%d%% period=\"%s\" forecast=\"%s\" code=%d\n",
                        rainChance, bestPeriodTime.c_str(), shortFc.c_str(), weatherCode);
          Serial.printf("Today: maxRain=%d%% rainStart=%s hours=%d\n",
                        todayMaxRainChance, rainExpectedToday ? rainStartTime : "none", todayForecastCount);
        } else {
          Serial.printf("NWS hourly forecast JSON parse failed: %s\n", err.c_str());
        }
      } else {
        Serial.printf("NWS hourly forecast HTTP error: %d\n", code);
        if (code == 301 || code == 404) rainForecastUrl = "";
      }
      gpHttp.end();
    }

    if (currentTemp != 0.0f) weatherValid = true;
    lastWeatherFetch = millis();
  }

  // --- NWS daily forecast: today's high/low temps ---
  {
    if (rainForecastDailyUrl.length() == 0) {
      resolveGridpoint();
    }

    if (rainForecastDailyUrl.length() > 0) {
      WiFiClientSecure dailyClient;
      dailyClient.setInsecure();
      dailyClient.setTimeout(20000);
      HTTPClient dailyHttp;
      dailyHttp.begin(dailyClient, rainForecastDailyUrl);
      dailyHttp.setTimeout(20000);
      dailyHttp.useHTTP10(true);  // same chunked-encoding fix as the hourly fetch
      dailyHttp.addHeader("User-Agent", NWS_USER_AGENT);
      dailyHttp.addHeader("Accept", "application/geo+json");
      int code = dailyHttp.GET();

      if (code == 200) {
        // Same margin fix as the hourly filter above — was sized to the exact
        // theoretical minimum (~160B), which risks silent truncation.
        StaticJsonDocument<512> filter;
        filter["properties"]["periods"][0]["startTime"] = true;
        filter["properties"]["periods"][0]["isDaytime"] = true;
        filter["properties"]["periods"][0]["temperature"] = true;

        DynamicJsonDocument doc(4096);
        PatientStreamReader dailyReader(dailyHttp.getStream(), dailyHttp);
        DeserializationError err = deserializeJson(doc, dailyReader,
                                                     DeserializationOption::Filter(filter));
        if (!err) {
          todayHighValid = false;
          todayLowValid  = false;

          JsonArray periods = doc["properties"]["periods"].as<JsonArray>();
          for (JsonObject period : periods) {
            const char* startIso = period["startTime"] | "";
            char pDate[9];
            parseNwsIsoTime(startIso, pDate, nullptr);
            if (strcmp(pDate, todayDate) != 0) break;  // periods are chronological — stop once past today

            bool isDaytime = period["isDaytime"] | false;
            int  temp      = period["temperature"] | 0;
            if (isDaytime) {
              if (!todayHighValid || temp > todayHighTemp) { todayHighTemp = temp; todayHighValid = true; }
            } else {
              if (!todayLowValid || temp < todayLowTemp)   { todayLowTemp  = temp; todayLowValid  = true; }
            }
          }
          Serial.printf("Today: high=%s low=%s\n",
                        todayHighValid ? String(todayHighTemp).c_str() : "n/a",
                        todayLowValid  ? String(todayLowTemp).c_str()  : "n/a");
        } else {
          Serial.printf("NWS daily forecast JSON parse failed: %s\n", err.c_str());
        }
      } else {
        Serial.printf("NWS daily forecast HTTP error: %d\n", code);
        if (code == 301 || code == 404) rainForecastDailyUrl = "";
      }
      dailyHttp.end();
    }
  }

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

  // Rest-of-day max, not just the next 4 hours — matches the forecast screen and
  // avoids showing a misleadingly low % when the near-term hours look dry but
  // rain is expected (or already happening) later/now in the day.
  snprintf(buf, sizeof(buf), "%d%%", todayMaxRainChance);
  tft.setTextSize(3);
  tft.drawString(buf, cx, rainAreaY + 22);
}

// ── Radar overlay: static basemap image, then live radar data drawn on top ──
// Drawn ON TOP of the basemap — skip any pixel that came out as pure background
// color (i.e. "no radar echo here") so the basemap underneath stays visible.
// Radar transparency: 0=invisible 255=fully opaque. 180=70%, 128=50%, 220=86%.
#define RADAR_ALPHA 110

int lastRadarDrawRow = -1;  // diagnostic: last row the callback actually received

// Blend one pixel's true RGB + alpha against the actual basemap pixel beneath
// it, weighted by real alpha (not PNGdec's flat-black-background shortcut,
// which produced muddy near-black values for partially-transparent pixels —
// that was the source of the speckled/streaky artifacts we used to see).
static inline uint16_t blendRadarPixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint16_t basePix) {
  if (a == 0) return basePix;
  uint8_t bR = ((basePix >> 11) & 0x1F) << 3;
  uint8_t bG = ((basePix >> 5) & 0x3F) << 2;
  uint8_t bB = (basePix & 0x1F) << 3;
  // Combine the pixel's real alpha with our own overlay strength so the radar
  // layer still reads as a translucent wash over the map rather than fully
  // opaque, matching the previous look for solid returns.
  uint16_t effA = (uint16_t)a * RADAR_ALPHA / 255;
  uint8_t outR = ((uint16_t)r * effA + (uint16_t)bR * (255 - effA)) / 255;
  uint8_t outG = ((uint16_t)g * effA + (uint16_t)bG * (255 - effA)) / 255;
  uint8_t outB = ((uint16_t)b * effA + (uint16_t)bB * (255 - effA)) / 255;
  return ((outR >> 3) << 11) | ((outG >> 2) << 5) | (outB >> 3);
}

int radarPngDraw(PNGDRAW *pDraw) {
  lastRadarDrawRow = pDraw->y;
  if (pDraw->y >= SCREEN_H) return 1;           // height guard
  int lineW = min((int)pDraw->iWidth, SCREEN_W); // width guard

  uint16_t blended[SCREEN_W];
  uint8_t *src = pDraw->pPixels;

  if (pDraw->iPixelType == PNG_PIXEL_INDEXED) {
    // We now request image/png8 from the WMS server: an indexed/palette PNG
    // instead of 32-bit RGBA. Each pixel is a small index (packed iBpp bits
    // per pixel) into pDraw->pPalette: bytes 0..767 are 256 RGB triples,
    // bytes 768..1023 are per-color alpha (only meaningful if iHasAlpha —
    // populated from the file's tRNS chunk; otherwise every color is opaque).
    uint8_t *pal = pDraw->pPalette;
    for (int x = 0; x < lineW; x++) {
      uint8_t idx;
      switch (pDraw->iBpp) {
        case 8: idx = src[x]; break;
        case 4: idx = (x & 1) ? (src[x >> 1] & 0x0F) : (src[x >> 1] >> 4); break;
        case 2: idx = (src[x >> 2] >> ((3 - (x & 3)) * 2)) & 0x03; break;
        case 1: idx = (src[x >> 3] >> (7 - (x & 7))) & 0x01; break;
        default: idx = 0; break;
      }
      uint8_t a = pDraw->iHasAlpha ? pal[768 + idx] : 255;
      uint16_t basePix = pgm_read_word(&basemap_img[pDraw->y * SCREEN_W + x]);
      blended[x] = blendRadarPixel(pal[idx * 3 + 0], pal[idx * 3 + 1], pal[idx * 3 + 2], a, basePix);
    }
  } else {
    // Fallback in case the server ever doesn't honor image/png8 and sends
    // full truecolor+alpha instead (pixelType 6, 4 bytes/pixel: R,G,B,A).
    for (int x = 0; x < lineW; x++) {
      uint16_t basePix = pgm_read_word(&basemap_img[pDraw->y * SCREEN_W + x]);
      blended[x] = blendRadarPixel(src[x * 4 + 0], src[x * 4 + 1], src[x * 4 + 2], src[x * 4 + 3], basePix);
    }
  }
  // One SPI transaction per row instead of one per run of non-background
  // pixels — a busy radar row could previously trigger dozens of tiny writes.
  tft.pushImage(0, pDraw->y, lineW, 1, blended);
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

  int MAX_BYTES = max(40000, (int)ESP.getMaxAllocHeap() - 20000);
  if (MAX_BYTES > 300000) MAX_BYTES = 300000;
  int bufCap = knownLength ? min(len, MAX_BYTES) : MAX_BYTES;

  if ((int)ESP.getMaxAllocHeap() < bufCap + 10000) {
    Serial.printf("Not enough heap: largest=%u need=%d\n", ESP.getMaxAllocHeap(), bufCap);
    http.end(); return false;
  }
  Serial.printf("Heap: free=%u largest=%u bufCap=%d\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap(), bufCap);

  uint8_t *buf = (uint8_t*)malloc(bufCap);
  if (!buf) {
    Serial.println("malloc failed");
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  int received = 0;
  bool hitCap = false;
  unsigned long readStart = millis();
  while (millis() - readStart < 20000) {
    if (knownLength && received >= len) break;
    if (received >= bufCap) { hitCap = true; break; }

    int avail = stream->available();
    if (avail > 0) {
      int toRead = min(avail, bufCap - received);
      received += stream->readBytes(buf + received, toRead);
    } else if (!http.connected()) {
      // Connection reports closed, but a few final (already TLS-decrypted) bytes
      // can still be sitting in the stream's local buffer right at the disconnect
      // boundary — give it one more moment before concluding we're truly done.
      // Silently losing just the tail of the file is exactly what let PNGdec run
      // out of data mid-decode even though this function reported success.
      delay(10);
      if (stream->available() <= 0) break;
    } else {
      delay(5);
    }
  }
  http.end();

  // For known-length responses, receiving fewer bytes than promised is incomplete.
  // For unknown-length responses (the common case here — WMS over HTTP/1.0 rarely
  // sends Content-Length), hitting the buffer cap before the connection closed on
  // its own means we stopped mid-stream, not at the real end of the image. Treat
  // that the same as incomplete rather than handing PNGdec a truncated buffer —
  // that's what was producing "top half fine, garbage below" on larger radar frames.
  if (received == 0 || (knownLength && received != len) || (!knownLength && hitCap)) {
    Serial.printf("Incomplete download (%d bytes received, cap=%d, hitCap=%d)\n",
                  received, bufCap, hitCap);
    free(buf);
    return false;
  }
  Serial.printf("Bytes received: %d\n", received);

  *outBuf = buf;
  *outLen = received;
  return true;
}

// Standard CRC32 (as used by PNG chunk trailers), computed bit-by-bit — no
// lookup table needed for a one-off ~11KB check.
uint32_t crc32_png(const uint8_t *buf, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= buf[i];
    for (int k = 0; k < 8; k++) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (0 - (crc & 1)));
    }
  }
  return crc ^ 0xFFFFFFFF;
}

// Decodes an already-downloaded PNG buffer via PNGdec using `drawCb`, then frees it.
bool decodePng(uint8_t *buf, int len, PNG_DRAW_CALLBACK *drawCb) {
  bool ok = false;
  // Reset up front, not just right before decode() — if we bail out early
  // (e.g. a CRC mismatch below, before decode is ever attempted), showRadar()
  // must not see a stale row count left over from a previous successful call
  // and mistakenly accept this failure as "close enough".
  lastRadarDrawRow = -1;

  // Dump both ends of the buffer as hex. A valid PNG must start with the 8-byte
  // signature 89 50 4E 47 0D 0A 1A 0A and its very last 12 bytes must be the IEND
  // chunk: 00 00 00 00 49 45 4E 44 AE 42 60 82. If that tail is missing/different
  // despite the byte count matching what the server declared, the file itself
  // (as generated/sent by the WMS server) is incomplete — not a download bug.
  {
    char hex[80];
    int n = min(len, 16);
    int o = 0;
    for (int i = 0; i < n; i++) o += snprintf(hex + o, sizeof(hex) - o, "%02X ", buf[i]);
    Serial.printf("PNG buf head (%d bytes): %s\n", len, hex);
    o = 0;
    int tailStart = max(0, len - 16);
    for (int i = tailStart; i < len; i++) o += snprintf(hex + o, sizeof(hex) - o, "%02X ", buf[i]);
    Serial.printf("PNG buf tail: %s\n", hex);
  }

  // Walk the top-level chunk table ourselves, independent of PNGdec, so we can
  // see the file's actual structure (chunk types, sizes, how many IDAT chunks,
  // total compressed bytes) rather than inferring it from PNGdec's behavior.
  // Also verify each chunk's CRC32 trailer — our earlier checks (byte count,
  // head/tail hex, chunk framing) only prove the file is structurally
  // well-formed, not that every byte in the middle arrived intact. A single
  // bit flipped somewhere inside an 11KB IDAT payload during a rare WiFi/TLS
  // hiccup would still pass all of those checks but leave the compressed
  // stream genuinely undecodable partway through — which is a much better
  // explanation for occasional failures well before the end of the image
  // (e.g. row 129 of 240) than a library bug, since the library-bug pattern
  // we've confirmed only ever costs the last handful of rows (235-239).
  bool crcOk = true;
  {
    Serial.println("PNG chunk walk:");
    int pos = 8;  // skip the 8-byte signature
    long idatTotal = 0;
    int idatCount = 0;
    while (pos + 8 <= len) {
      uint32_t chunkLen = ((uint32_t)buf[pos] << 24) | ((uint32_t)buf[pos + 1] << 16) |
                          ((uint32_t)buf[pos + 2] << 8) | buf[pos + 3];
      char type[5] = { (char)buf[pos + 4], (char)buf[pos + 5], (char)buf[pos + 6], (char)buf[pos + 7], 0 };
      bool haveCrc = (pos + 8 + (int)chunkLen + 4 <= len);
      bool chunkCrcOk = true;
      if (haveCrc) {
        uint32_t computed = crc32_png(&buf[pos + 4], chunkLen + 4);  // type + data
        uint32_t stored = ((uint32_t)buf[pos + 8 + chunkLen] << 24) | ((uint32_t)buf[pos + 9 + chunkLen] << 16) |
                          ((uint32_t)buf[pos + 10 + chunkLen] << 8) | buf[pos + 11 + chunkLen];
        chunkCrcOk = (computed == stored);
      }
      Serial.printf("  %-4s len=%u offset=%d crc=%s\n", type, (unsigned)chunkLen, pos,
                    haveCrc ? (chunkCrcOk ? "ok" : "MISMATCH") : "n/a");
      if ((strcmp(type, "IDAT") == 0 || strcmp(type, "IHDR") == 0) && !chunkCrcOk) crcOk = false;
      if (strcmp(type, "IDAT") == 0) { idatTotal += chunkLen; idatCount++; }
      if (strcmp(type, "IEND") == 0) break;
      if (chunkLen > (uint32_t)len) { Serial.println("  (bad length — aborting walk)"); break; }
      pos += 8 + chunkLen + 4;  // header(8) + data + CRC(4)
    }
    Serial.printf("PNG chunk walk: %d IDAT chunk(s), %ld total compressed bytes, file=%d bytes\n",
                  idatCount, idatTotal, len);
  }

  if (!crcOk) {
    // A critical chunk's CRC didn't match its data — the transfer corrupted
    // something mid-stream. Don't bother attempting to decode known-bad
    // data; just report it the same way as a failed decode.
    Serial.println("PNG chunk CRC mismatch — corrupted transfer, skipping decode");
    free(buf);
    return false;
  }

  int rc = png.openRAM(buf, len, drawCb);
  if (rc == PNG_SUCCESS) {
    tft.setSwapBytes(true);
    Serial.printf("PNG info: %dx%d bpp=%d pixelType=%d hasAlpha=%d interlaced=%d\n",
                  png.getWidth(), png.getHeight(), png.getBpp(), png.getPixelType(),
                  png.hasAlpha(), png.isInterlaced());
    Serial.printf("Heap before decode: free=%u largest=%u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    lastRadarDrawRow = -1;
    rc = png.decode(NULL, 0);
    png.close();
    Serial.printf("PNG decode result: %d (last row drawn: %d of %d)\n", rc, lastRadarDrawRow, png.getHeight());
    // decode() returning non-zero means it stopped before finishing every scanline
    // (corrupt/unsupported data, or ran out of contiguous heap mid-decode). Whatever
    // rows the draw callback already pushed are still on screen — the caller needs
    // to know this so it can clean that up rather than leaving a garbled partial frame.
    ok = (rc == PNG_SUCCESS);
    if (!ok) {
      Serial.println("PNG decode did not complete — image is likely partially garbled");
    }
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
    // Tried format=image/png8 (indexed palette) to shrink the payload, but
    // this server silently ignores it and always returns full 32-bit RGBA
    // regardless (confirmed via pixelType in the decode log) — PNG8 isn't
    // supported/enabled for this layer on NOAA's GeoServer instance. Back to
    // plain PNG; radarPngDraw() still has an indexed-pixel code path ready
    // in case that ever changes, but the fallback (RGBA) branch is what
    // actually runs.
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
    bool decoded = decodePng(radarBuf, radarLen, radarPngDraw);
    // Confirmed root cause by reading PNGdec's DecodePNG() source directly:
    // its success path only fires when a single inflate() call BOTH returns
    // Z_STREAM_END AND exactly finishes filling the current scanline's output
    // buffer on that same call. If the final scanline finishes on one call
    // (Z_OK, avail_out==0) and the stream's actual end arrives on the next
    // call, that success check never matches. The outer loop then thinks it
    // still needs more image rows, tries to read another chunk header past
    // IEND, hits real end-of-file, and hard-fails with PNG_DECODE_ERROR —
    // even though every real pixel row had already decoded correctly. This
    // consistently costs the last handful of rows (observed 235-239 of 240).
    // It's a genuine library bug, not a download/data problem (every file
    // we've inspected byte-for-byte has been complete and valid).
    //
    // Rejecting every non-success decode was tried and made things worse:
    // decode essentially never returns a clean full success on this device
    // (it consistently lands around row 235-239 of 240), so requiring
    // perfection meant the radar screen failed almost every time. Back to
    // accepting "got within a few rows of the bottom" as good enough. One
    // report of visible garbage on an accepted near-complete decode came in
    // without a photo to confirm it was real corruption rather than faint
    // low-alpha radar texture at a low RADAR_ALPHA setting — if it recurs
    // with a photo showing genuine garbage (not just sparse echo dots),
    // that's real evidence this tolerance is unsafe and needs a different
    // approach (e.g. discarding the frame entirely rather than showing it).
    const int RADAR_ROW_TOLERANCE = 8;
    bool closeEnough = !decoded && lastRadarDrawRow >= SCREEN_H - RADAR_ROW_TOLERANCE;
    if (closeEnough) {
      Serial.printf("Radar decode close enough (row %d of %d) — accepting\n", lastRadarDrawRow, SCREEN_H);
    }
    if (!decoded && !closeEnough) {
      // Decode stopped well short — wipe out whatever partial/garbled rows
      // radarPngDraw() already pushed by redrawing a clean basemap, then say so
      // instead of silently leaving a corrupted frame on screen.
      tft.pushImage(0, 0, BASEMAP_W, BASEMAP_H, basemap_img);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(COL_WHITE, COL_BG);
      tft.setTextSize(2);
      tft.drawString("Radar decode error", SCREEN_W / 2, SCREEN_H / 2);
    }
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

// ── Today's forecast screen ───────────────────────────────────────────────────
// Reached with a single tap from the radar screen; a tap here returns home.
void drawForecastScreen() {
  tft.fillScreen(COL_BG);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setTextSize(2);
  tft.drawString("TODAY'S FORECAST", SCREEN_W / 2, 8);
  tft.drawFastHLine(10, 28, SCREEN_W - 20, COL_DIVIDER);

  if (!forecastDataValid) {
    tft.setTextSize(1);
    tft.drawString("Fetching...", SCREEN_W / 2, 100);
    return;
  }

  char buf[40];

  // High / Low — orange for high, blue for low, twice the previous font size
  tft.setTextSize(2);
  tft.setTextDatum(TC_DATUM);

  tft.setTextColor(TFT_ORANGE, COL_BG);
  if (todayHighValid) snprintf(buf, sizeof(buf), "High %dF", todayHighTemp);
  else                 snprintf(buf, sizeof(buf), "High --");
  tft.drawString(buf, SCREEN_W / 4, 32);

  tft.setTextColor(TFT_BLUE, COL_BG);
  if (todayLowValid) snprintf(buf, sizeof(buf), "Low %dF", todayLowTemp);
  else                snprintf(buf, sizeof(buf), "Low --");
  tft.drawString(buf, 3 * SCREEN_W / 4, 32);

  // Rain chance — also doubled in size
  tft.setTextColor(0x07FF, COL_BG);
  snprintf(buf, sizeof(buf), "Rain chance: %d%%", todayMaxRainChance);
  tft.drawString(buf, SCREEN_W / 2, 54);

  if (rainExpectedToday) {
    tft.setTextColor(COL_FEELS, COL_BG);
    snprintf(buf, sizeof(buf), "Rain starts ~%s", rainStartTime);
  } else {
    tft.setTextColor(COL_HIGH_TIDE, COL_BG);
    snprintf(buf, sizeof(buf), "No rain expected today");
  }
  tft.drawString(buf, SCREEN_W / 2, 78);

  // Hour-by-hour rain chance for the rest of today — larger font means fewer
  // rows fit (6 instead of 8), but still covers a useful near-term window.
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(2);
  int y = 100;
  const int rowH = 20;
  const int DISPLAY_ROWS = 6;
  int rows = min(todayForecastCount, DISPLAY_ROWS);
  for (int i = 0; i < rows; i++) {
    uint16_t col = todayForecast[i].isRain ? COL_LOW_TIDE : COL_WHITE;
    tft.setTextColor(col, COL_BG);
    snprintf(buf, sizeof(buf), "%-7s %3d%%  %s", todayForecast[i].time, todayForecast[i].pop,
             todayForecast[i].isRain ? "Rain" : "--");
    tft.drawString(buf, 24, y);
    y += rowH;
  }

  // Only worth mentioning the list being empty when rain was actually expected —
  // on a no-rain day, "No rain expected today" above already says everything needed.
  if (rows == 0 && rainExpectedToday) {
    tft.setTextSize(1);
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.drawString("No more hours left today.", 24, y);
  }
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
  // One-time check: does this board actually have PSRAM wired up? If
  // getPsramSize() reports 0, there's no external RAM available no matter
  // what — the radar decode is stuck working within internal SRAM only. If
  // it reports several MB, a full-image-buffer PNG decode (a more robust,
  // battle-tested library instead of the line-streaming PNGdec) becomes a
  // real option.
  Serial.printf("PSRAM size: %u bytes (found=%d)\n", ESP.getPsramSize(), psramFound());

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
        if (!radarMode && !forecastMode) drawAll();
        Serial.println("Screen ON (touch wake)");
      }

      // Tap sequence: home --(double-tap)--> radar --(tap)--> forecast --(tap)--> home
      if (forecastMode) {
        // Any tap while viewing the forecast screen returns to the home screen.
        Serial.println("Tap detected — exiting forecast screen");
        forecastMode = false;
        drawAll();
        lastTapMs = now;
      } else if (radarMode) {
        // Any tap while viewing radar moves on to the forecast screen.
        Serial.println("Tap detected — showing forecast");
        radarMode      = false;
        forecastMode   = true;
        forecastStartMs = now;
        drawForecastScreen();
        lastTapMs = now;
      } else if (gap <= DOUBLE_TAP_WINDOW_MS) {
        Serial.println("Double-tap detected — showing radar");
        radarMode    = true;
        radarStartMs = now;
        showRadar();
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

  // --- Forecast screen timeout ---
  if (forecastMode && (now - forecastStartMs >= FORECAST_DURATION_MS)) {
    forecastMode = false;
    lastTouchMs = now;
    Serial.println("Forecast timeout — returning to normal display");
    drawAll();
  }

  // --- Sleep timeout (paused while viewing radar or forecast) ---
  if (screenOn && !radarMode && !forecastMode && (now - lastTouchMs >= SLEEP_MS)) {
    digitalWrite(BACKLIGHT_PIN, LOW);
    screenOn = false;
    Serial.println("Screen OFF (sleep)");
  }

  if (!screenOn) { delay(50); return; }

  if (!radarMode && !forecastMode) {
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