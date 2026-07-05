/*
 * TideDisplay.ino
 * ESP32 (ESP-32E) + 3.2" ST7789P3 TFT display (Hosyond)
 * Shows: Great Wave graphic | Next 4 tide times (H/L) | Current temp & feels-like
 * Location: Hilton Head Island, SC 29928  (NOAA Station 8665530)
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>
#include <WiFiClientSecure.h>
#include "wave_img.h"

// ── WiFi ──────────────────────────────────────────────────────────────────────
const char* SSID     = "Dexter5G";
const char* PASSWORD = "maxmaddie";

// ── NTP ───────────────────────────────────────────────────────────────────────
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET = -18000;   // EST = UTC-5
const int   DST_OFFSET =  3600;   // EDT = +1hr DST

// ── NOAA Station: Hilton Head Island SC ───────────────────────────────────────
const char* NOAA_STATION = "8665530";

// ── Open-Meteo weather ────────────────────────────────────────────────────────
const char* WEATHER_URL =
  "https://api.open-meteo.com/v1/forecast"
  "?latitude=32.1354&longitude=-80.9274"
  "&current=temperature_2m,apparent_temperature,weather_code"
  "&hourly=precipitation_probability"
  "&temperature_unit=fahrenheit"
  "&timezone=America%2FNew_York"
  "&forecast_hours=1";

// ── Refresh intervals ─────────────────────────────────────────────────────────
const unsigned long TIDE_REFRESH_MS    = 6UL * 3600UL * 1000UL;  // 6 hrs
const unsigned long WEATHER_REFRESH_MS = 10UL * 60UL * 1000UL;   // 10 min

// ── Display ───────────────────────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();



const int DIVIDER_X = 192;   // 60% of 320
const int WAVE_X    = 2;
const int TIDE_X    = 104;
const int SCREEN_W  = 320;
const int SCREEN_H  = 240;

// ── Colours (RGB565) ──────────────────────────────────────────────────────────
#define COL_BG        0x0000
#define COL_WHITE     0xFFFF
#define COL_LOW_TIDE  0x6CC0   // olive green
#define COL_HIGH_TIDE 0x34BF   // steel blue
#define COL_FEELS     0xF800   // bright red
#define COL_DIVIDER   0x2945   // dim grey

// ── Data ──────────────────────────────────────────────────────────────────────
struct TideEvent {
  char time[12];
  char date[10];
  bool isHigh;
};

TideEvent tides[4];
int  tideCount      = 0;
float currentTemp   = 0.0f;
float feelsLikeTemp = 0.0f;
int   weatherCode   = 0;
int   rainChance    = 0;
bool  weatherValid  = false;

unsigned long lastTideFetch    = 0;
unsigned long lastWeatherFetch = 0;

// ── Parse NOAA time string "YYYY-MM-DD HH:MM" ────────────────────────────────
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

  // Use HTTPS via WiFiClientSecure for NOAA (they redirect HTTP to HTTPS)
  WiFiClientSecure secClient;
  secClient.setInsecure();  // skip cert verification

  HTTPClient http;
  String url = String("https://api.tidesandcurrents.noaa.gov/api/prod/datagetter") +
    "?begin_date=" + beginDate + "&end_date=" + endDate +
    "&station=" + NOAA_STATION +
    "&product=predictions&datum=MLLW&time_zone=lst_ldt"
    "&interval=hilo&units=english&application=tide_display&format=json";
  Serial.println("NOAA HTTPS: " + url);

  http.begin(secClient, url);
  http.setTimeout(20000);
  int code = http.GET();
  Serial.printf("NOAA HTTPS code: %d\n", code);
  if (code != 200) {
    if (code > 0) Serial.println(http.getString().substring(0,200));
    http.end();
    return;
  }

  String body = http.getString();
  http.end();
  Serial.printf("Body len=%d\n", body.length());

  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, body)) { Serial.println("JSON err"); return; }

  JsonArray predictions = doc["predictions"].as<JsonArray>();
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
  WiFiClientSecure secClient2;
  secClient2.setInsecure();
  HTTPClient http;
  http.begin(secClient2, WEATHER_URL);
  http.setTimeout(20000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Weather HTTP error: %d\n", code);
    http.end();
    return;
  }
  String body = http.getString();
  http.end();

  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, body)) return;

  currentTemp   = doc["current"]["temperature_2m"].as<float>();
  feelsLikeTemp = doc["current"]["apparent_temperature"].as<float>();
  weatherCode   = doc["current"]["weather_code"].as<int>();
  // First hourly precipitation probability = current hour rain chance
  rainChance    = doc["hourly"]["precipitation_probability"][0].as<int>();
  weatherValid  = true;
  Serial.printf("Weather: %.1fF feels %.1fF\n", currentTemp, feelsLikeTemp);
  lastWeatherFetch = millis();
}

// ── Draw wave image ───────────────────────────────────────────────────────────
void drawWave() {
  int y0 = (SCREEN_H - WAVE_H) / 2;
  if (y0 < 0) y0 = 0;
  tft.pushImage(WAVE_X, y0, WAVE_W, WAVE_H, (uint16_t*)wave_img);
}

// ── Draw tide panel ───────────────────────────────────────────────────────────
void drawTides() {
  tft.fillRect(TIDE_X, 0, DIVIDER_X - TIDE_X, SCREEN_H, COL_BG);
  tft.setTextDatum(TL_DATUM);
  if (tideCount == 0) {
    tft.setTextColor(COL_WHITE, COL_BG);
    tft.setTextSize(1);
    tft.drawString("Fetching...", TIDE_X + 4, 16);
    return;
  }

  // Each row = full height / 4, content centered vertically in each row
  int rowH = SCREEN_H / 4;  // 60px per row — fills all 240px
  for (int i = 0; i < tideCount; i++) {
    int idx = tideCount - 1 - i;   // reverse order: most recent at top
    int y = i * rowH + 4;          // 4px top padding within each row
    uint16_t col = tides[idx].isHigh ? COL_HIGH_TIDE : COL_LOW_TIDE;
    tft.setTextColor(col, COL_BG);
    tft.setTextSize(1);
    tft.drawString(tides[idx].isHigh ? "HIGH" : "LOW", TIDE_X + 4, y);
    tft.drawString(tides[idx].date, TIDE_X + 4, y + 12);
    tft.setTextSize(2);
    tft.drawString(tides[idx].time, TIDE_X + 2, y + 24);
    // Row divider (skip last)
    if (i < tideCount - 1) {
      tft.drawFastHLine(TIDE_X, (i + 1) * rowH, DIVIDER_X - TIDE_X, COL_DIVIDER);
    }
  }
}


// ── Fancy Cartoon Weather Icons ──────────────────────────────────────────────
// Drawn in ~56x56 box centred on (cx,cy)

// Helper: filled rounded cloud shape
void drawCloud(int cx, int cy, uint16_t col, uint16_t outline) {
  tft.fillCircle(cx-12, cy+4,  9, col);
  tft.fillCircle(cx+2,  cy+4,  9, col);
  tft.fillCircle(cx+14, cy+4,  8, col);
  tft.fillCircle(cx-4,  cy-4, 11, col);
  tft.fillCircle(cx+8,  cy-2,  9, col);
  tft.fillRect(cx-21, cy+4, 36, 10, col);
  // Highlight puff on top
  tft.fillCircle(cx-4, cy-4, 5, col | 0x2000);
  // Outline
  tft.drawCircle(cx-12, cy+4,  9, outline);
  tft.drawCircle(cx+2,  cy+4,  9, outline);
  tft.drawCircle(cx+14, cy+4,  8, outline);
  tft.drawCircle(cx-4,  cy-4, 11, outline);
  tft.drawCircle(cx+8,  cy-2,  9, outline);
}

void drawIconSun(int cx, int cy) {
  // Glow ring
  tft.fillCircle(cx, cy, 22, 0xFE60);
  // Rays - pre-computed 8 directions
  static const int8_t ix[] = { 0,13,18,13, 0,-13,-18,-13};
  static const int8_t iy[] = {-18,-13,0,13,18,13, 0,-13};
  static const int8_t ox[] = { 0,18,25,18, 0,-18,-25,-18};
  static const int8_t oy[] = {-25,-18,0,18,25,18, 0,-18};
  for (int i=0;i<8;i++) {
    tft.drawLine(cx+ix[i],cy+iy[i],cx+ox[i],cy+oy[i],0xFD00);
    tft.drawLine(cx+ix[i]+1,cy+iy[i],cx+ox[i]+1,cy+oy[i],0xFD00);
    tft.drawLine(cx+ix[i],cy+iy[i]+1,cx+ox[i],cy+oy[i]+1,0xFD00);
  }
  // Main circle with gradient effect
  tft.fillCircle(cx, cy, 18, 0xFEE0);
  tft.fillCircle(cx, cy, 16, 0xFFE0);
  tft.fillCircle(cx-3, cy-3, 6, 0xFFFF);  // highlight
  // Face
  tft.fillCircle(cx-6, cy-3, 3, 0x8400);  // left eye
  tft.fillCircle(cx+6, cy-3, 3, 0x8400);  // right eye
  tft.fillCircle(cx-5, cy-4, 1, 0xFFFF);  // eye shine
  tft.fillCircle(cx+7, cy-4, 1, 0xFFFF);
  // Big smile arc
  for (int dx=-7;dx<=7;dx++) tft.drawPixel(cx+dx, cy+5+(dx*dx)/8, 0x8400);
  for (int dx=-7;dx<=7;dx++) tft.drawPixel(cx+dx, cy+6+(dx*dx)/8, 0x8400);
  // Cheek blush
  tft.fillCircle(cx-10, cy+4, 3, 0xFB8C);
  tft.fillCircle(cx+10, cy+4, 3, 0xFB8C);
}

void drawIconCloud(int cx, int cy) {
  // Shadow
  drawCloud(cx+2, cy+2, 0x4208, 0x4208);
  // Main cloud - white/light blue
  drawCloud(cx, cy, 0xEF7D, 0x8C51);
  // Sleepy face
  tft.fillCircle(cx-7, cy+2, 2, 0x6B4D);
  tft.fillCircle(cx+5, cy+2, 2, 0x6B4D);
  // Zzz
  tft.setTextColor(0x8C51, 0x0000);
  tft.setTextSize(1);
  tft.setCursor(cx+10, cy-10);
  tft.print("z");
  tft.setCursor(cx+14, cy-16);
  tft.print("Z");
}

void drawIconPartlyCloud(int cx, int cy) {
  // Sun (behind, offset up-right)
  int sx=cx+10, sy=cy-12;
  tft.fillCircle(sx, sy, 16, 0xFE60);
  tft.fillCircle(sx, sy, 13, 0xFFE0);
  tft.fillCircle(sx-3, sy-3, 4, 0xFFFF);
  static const int8_t ix[]={ 0,9,13, 9, 0,-9,-13,-9};
  static const int8_t iy[]={-13,-9, 0, 9,13, 9,  0,-9};
  static const int8_t ox[]={ 0,13,18,13, 0,-13,-18,-13};
  static const int8_t oy[]={-18,-13,0,13,18,13,  0,-13};
  for(int i=0;i<8;i++)
    tft.drawLine(sx+ix[i],sy+iy[i],sx+ox[i],sy+oy[i],0xFD00);
  // Cloud in front
  drawCloud(cx-6, cy+6, 0xF7DE, 0x8C51);
  tft.fillCircle(cx-9, cy+8, 2, 0x6B4D);
  tft.fillCircle(cx+1, cy+8, 2, 0x6B4D);
  for(int dx=-4;dx<=4;dx++) tft.drawPixel(cx-4+dx, cy+13+(dx*dx)/6, 0x6B4D);
}

void drawIconRain(int cx, int cy) {
  // Dark grey cloud
  drawCloud(cx, cy-8, 0x7BCF, 0x4208);
  // Lightning hint inside cloud
  tft.fillTriangle(cx+2,cy-12, cx-2,cy-6, cx+2,cy-6, 0xFFE0);
  // Raindrops - teardrop shapes with blue gradient
  int dx[] = {-14,-6,2,10,-10,0,8};
  int dy[] = {8,6,8,6,16,18,16};
  for(int i=0;i<7;i++){
    tft.fillCircle(cx+dx[i], cy+dy[i], 3, 0x041F);
    tft.fillCircle(cx+dx[i], cy+dy[i]-4, 2, 0x041F);
    tft.fillCircle(cx+dx[i]+1, cy+dy[i]-1, 1, 0x9DFF); // highlight
  }
}

void drawIconStorm(int cx, int cy) {
  // Very dark cloud
  drawCloud(cx, cy-10, 0x4208, 0x2104);
  // Second cloud layer for drama
  tft.fillCircle(cx-8, cy-6, 7, 0x528A);
  tft.fillCircle(cx+8, cy-6, 7, 0x528A);
  // Bold lightning bolt
  uint16_t lc = 0xFFE0;
  tft.fillTriangle(cx+4,cy-2, cx-6,cy+10, cx+2,cy+10, lc);
  tft.fillTriangle(cx+2,cy+10, cx-8,cy+22, cx+6,cy+14, lc);
  tft.drawTriangle(cx+4,cy-2, cx-6,cy+10, cx+2,cy+10, 0xFD00);
  tft.drawTriangle(cx+2,cy+10, cx-8,cy+22, cx+6,cy+14, 0xFD00);
  // Rain drops
  int dx[]={-14,10}; int dy[]={12,14};
  for(int i=0;i<2;i++){
    tft.fillCircle(cx+dx[i],cy+dy[i],3,0x041F);
    tft.fillCircle(cx+dx[i],cy+dy[i]-4,2,0x041F);
  }
}

void drawIconFog(int cx, int cy) {
  // Sleepy sun above fog
  tft.fillCircle(cx, cy-16, 10, 0xFFE0);
  tft.fillCircle(cx-2,cy-18, 3, 0xFFFF);
  tft.fillCircle(cx-3,cy-16,2,0x8400);
  tft.fillCircle(cx+3,cy-16,2,0x8400);
  // Fog bands with varying opacity/width
  int widths[]={34,28,34,22,30};
  int ys[]={-4,4,12,20,28};
  uint16_t cols[]={0xC618,0xA534,0xC618,0xA534,0xC618};
  for(int i=0;i<5;i++){
    tft.fillRoundRect(cx-widths[i]/2, cy+ys[i]-3, widths[i], 6, 3, cols[i]);
    tft.drawRoundRect(cx-widths[i]/2, cy+ys[i]-3, widths[i], 6, 3, 0x8410);
  }
}

void drawWeatherIcon(int cx, int cy, int code) {
  if (code == 0)                              drawIconSun(cx, cy);
  else if (code <= 2)                         drawIconPartlyCloud(cx, cy);
  else if (code == 3)                         drawIconCloud(cx, cy);
  else if (code == 45 || code == 48)          drawIconFog(cx, cy);
  else if (code >= 51 && code <= 82)          drawIconRain(cx, cy);
  else if (code >= 95)                        drawIconStorm(cx, cy);
  else                                        drawIconCloud(cx, cy);
}

// ── Draw weather panel ────────────────────────────────────────────────────────
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

  char buf[16];

  // Current temp — large white
  snprintf(buf, sizeof(buf), "%d", (int)roundf(currentTemp));
  // Draw temp + degF on one line at size 3
  char tempLine[20];
  snprintf(tempLine, sizeof(tempLine), "%s F", buf);
  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setTextSize(3);
  tft.drawString(tempLine, cx, 55);

  // Divider
  tft.drawFastHLine(wx, 95, ww, COL_DIVIDER);

  // Feels like — smaller red
  tft.setTextColor(COL_WHITE, COL_BG);
  tft.setTextSize(1);
  tft.drawString("Feels Like", cx, 100);

  char flLine[20];
  snprintf(flLine, sizeof(flLine), "%d F", (int)roundf(feelsLikeTemp));
  tft.setTextColor(COL_FEELS, COL_BG);
  tft.setTextSize(2);
  tft.drawString(flLine, cx, 115);

  // Divider line above icon
  tft.drawFastHLine(wx, 145, ww, COL_DIVIDER);

  // Cartoon weather icon
  tft.fillRect(wx, 147, ww, 93, COL_BG);
  drawWeatherIcon(cx, 182, weatherCode);
  Serial.printf("Icon drawn: code=%d cx=%d cy=182\n", weatherCode, cx);

  // Rain chance below icon
  char rainBuf[16];
  snprintf(rainBuf, sizeof(rainBuf), "Rain: %d%%", rainChance);
  tft.setTextColor(0x07FF, COL_BG);  // cyan
  tft.setTextSize(1);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(rainBuf, cx, 228);
}

// ── Full redraw ───────────────────────────────────────────────────────────────
void drawAll() {
  Serial.println("drawAll() called");
  tft.fillScreen(COL_BG);
  drawWave();
  drawTides();
  drawWeather();
  Serial.println("drawAll() complete");
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== TideDisplay starting ===");

  // Backlight on
  pinMode(27, OUTPUT);
  digitalWrite(27, HIGH);

  pinMode(27, OUTPUT);
  digitalWrite(27, HIGH);  // backlight on
  delay(100);

  tft.init();
  tft.setRotation(3);
  tft.setSwapBytes(true);

  // Color flash - confirms display is alive
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

  // Sync time via NTP (needed for NOAA date)
  tft.fillScreen(COL_BG);
  tft.setTextColor(COL_WHITE, COL_BG);
  tft.drawString("Syncing time...", SCREEN_W/2, SCREEN_H/2);
  configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER);

  // Wait for NTP sync (up to 10s)
  time_t t = 0;
  for (int i = 0; i < 20 && t < 100000; i++) {
    delay(500);
    t = time(nullptr);
    Serial.printf("NTP t=%lu\n", t);
  }
  Serial.printf("Time synced: %s", ctime(&t));

  tft.fillScreen(COL_BG);
  tft.drawString("Fetching data...", SCREEN_W/2, SCREEN_H/2);

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

  bool redraw = false;

  if (now - lastTideFetch >= TIDE_REFRESH_MS) {
    fetchTides();
    redraw = true;
  }
  if (now - lastWeatherFetch >= WEATHER_REFRESH_MS) {
    fetchWeather();
    redraw = true;
  }
  if (redraw) drawAll();

  delay(30000);
}
