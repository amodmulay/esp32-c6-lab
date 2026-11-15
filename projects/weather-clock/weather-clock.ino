// Weather + clock for Waveshare ESP32-C6-Touch-LCD-1.47 (172x320 LCD)
// Time: NTP.  Location: IP geolocation (or override in secrets.h).  Weather: Open-Meteo (no API key).
#include <C6Board.h>  // shared screen + touch support (libraries/C6Board)
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "secrets.h"

const int W = C6_WIDTH;
int fitSize(const String &s, int maxSize);
uint16_t C_BG, C_TIME, C_DIM, C_TEMP, C_ACC, C_WARN, C_UP, C_CLOSED;

struct Weather {
  bool ok = false;
  float temp, feels, wind;
  int hum, code, isDay;
  float hi[3], lo[3];
  int dcode[3];
  String dayName[3];
} wx;

String city = "";
double lat = 0, lon = 0;
bool haveLoc = false;
int32_t utcOffset = 0;
unsigned long lastWeather = 0;
const unsigned long WEATHER_EVERY = 15UL * 60UL * 1000UL;

// ---- text slots: only repaint a row when its text changed ----
String cache[48];
void row(int slot, int y, int h, const String &s, int size, uint16_t fg, int align = 1) {
  if (cache[slot] == s) return;
  cache[slot] = s;
  gfx->fillRect(0, y, W, h, C_BG);
  gfx->setTextSize(size);
  gfx->setTextColor(fg, C_BG);
  int tw = s.length() * 6 * size;
  int x = align == 0 ? 6 : (align == 2 ? W - tw - 6 : (W - tw) / 2);
  if (x < 0) x = 0;
  gfx->setCursor(x, y + (h - 8 * size) / 2);
  gfx->print(s);
}

const char *wmo(int c) {
  if (c == 0) return "Clear";
  if (c == 1) return "Mostly clear";
  if (c == 2) return "Partly cloudy";
  if (c == 3) return "Overcast";
  if (c == 45 || c == 48) return "Fog";
  if (c >= 51 && c <= 57) return "Drizzle";
  if (c >= 61 && c <= 67) return "Rain";
  if (c >= 71 && c <= 77) return "Snow";
  if (c >= 80 && c <= 82) return "Showers";
  if (c == 85 || c == 86) return "Snow showers";
  if (c == 95) return "Thunderstorm";
  if (c == 96 || c == 99) return "Hail storm";
  return "--";
}

void status(const String &a, const String &b = "") {
  gfx->fillScreen(C_BG);
  for (auto &s : cache) s = "";
  row(0, 120, 24, a, 2, C_TIME);
  if (b.length()) row(1, 150, 16, b, 1, C_DIM);
}

// When both known networks are in range at boot, let the user pick: tap switches the highlight
// (orientation-independent), and the highlighted one is used after a few seconds of no touch.
int chooseWifi(const char *a, const char *b) {
  const char *names[2] = {a, b};
  int sel = 0, shown = -1, lastSec = -1;
  unsigned long deadline = millis() + 8000;
  for (auto &c : cache) c = "";
  gfx->fillScreen(C_BG);
  row(40, 8, 24, "Choose Wi-Fi", 2, C_TIME);
  while (millis() < deadline) {
    if (board_gesture()) { sel = 1 - sel; deadline = millis() + 5000; }
    if (sel != shown) {
      shown = sel;
      for (int i = 0; i < 2; i++) {
        int y = 56 + i * 96;
        bool on = i == sel;
        gfx->fillRoundRect(8, y, W - 16, 80, 10, on ? C_ACC : C_BG);
        gfx->drawRoundRect(8, y, W - 16, 80, 10, on ? C_ACC : C_DIM);
        String n = names[i];
        int sz = fitSize(n, 2);
        gfx->setTextSize(sz);
        gfx->setTextColor(on ? C_BG : C_TIME, on ? C_ACC : C_BG);
        gfx->setCursor(max(10, (W - (int)n.length() * 6 * sz) / 2), y + (80 - 8 * sz) / 2);
        gfx->print(n);
      }
    }
    int sec = (int)((deadline - millis() + 999) / 1000);
    if (sec != lastSec) { lastSec = sec; row(41, 262, 16, "Connecting in " + String(sec) + "s", 1, C_DIM); row(42, 282, 16, "tap to switch", 1, C_DIM); }
    delay(15);
  }
  return sel;
}

WiFiMulti wifiMulti;  // joins the strongest known network: home Wi-Fi, or the phone hotspot as a fallback

bool connectWifi() {
  if (String(WIFI_SSID).length() == 0 || String(WIFI_SSID) == "YOUR_WIFI_NAME") {
    status("Set Wi-Fi", "edit secrets.h");
    return false;
  }
  status("Wi-Fi...", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    Serial.printf("wifi disconnect reason=%d (201=AP not found, 2/15/202=auth/password, 204=handshake timeout)\n",
                  info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
#ifdef WIFI_SSID2
  bool seen1 = false, seen2 = false;
  int found = WiFi.scanNetworks();
  for (int i = 0; i < found; i++) {
    if (WiFi.SSID(i) == WIFI_SSID) seen1 = true;
    if (WiFi.SSID(i) == WIFI_SSID2) seen2 = true;
  }
  Serial.printf("known networks in range: %s=%d %s=%d\n", WIFI_SSID, seen1, WIFI_SSID2, seen2);
  if (seen1 && seen2) {
    int pick = chooseWifi(WIFI_SSID, WIFI_SSID2);
    Serial.printf("user chose %d\n", pick);
    status("Wi-Fi...", pick ? WIFI_SSID2 : WIFI_SSID);
    if (pick) wifiMulti.addAP(WIFI_SSID2, WIFI_PASSWORD2); else wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);
  } else {
    wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);
    wifiMulti.addAP(WIFI_SSID2, WIFI_PASSWORD2);
  }
#else
  wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);
#endif
  wifiMulti.run(20000);
  if (WiFi.status() == WL_CONNECTED) Serial.printf("wifi joined '%s'\n", WiFi.SSID().c_str());
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("wifi FAILED status=%d (1=no SSID found, 4=connect failed/bad password, 6=disconnected)\n", WiFi.status());
    WiFi.disconnect(true);
    delay(500);
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks();
    Serial.printf("scan found %d networks:\n", n);
    for (int i = 0; i < n; i++)
      Serial.printf("  '%s' ch%d rssi%d enc=%d%s\n", WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i), WiFi.encryptionType(i),
                    WiFi.SSID(i) == WIFI_SSID ? "  <-- yours" : "");
  }
  return WiFi.status() == WL_CONNECTED;
}

bool getLocation() {
#ifdef LAT_OVERRIDE
  lat = LAT_OVERRIDE; lon = LON_OVERRIDE; city = CITY_OVERRIDE; haveLoc = true;
  return true;
#else
  HTTPClient http;
  http.begin("http://ip-api.com/json/?fields=status,city,lat,lon");
  http.setTimeout(8000);
  int code = http.GET();
  Serial.printf("loc http=%d\n", code);
  if (code != 200) { http.end(); return false; }
  JsonDocument d;
  DeserializationError e = deserializeJson(d, http.getString());
  http.end();
  if (e || String((const char *)(d["status"] | "")) != "success") { Serial.println("loc parse fail"); return false; }
  lat = d["lat"]; lon = d["lon"]; city = String((const char *)(d["city"] | ""));
  haveLoc = true;
  return true;
#endif
}

bool getWeather() {
  if (!haveLoc && !getLocation()) return false;
  WiFiClientSecure client;
  client.setInsecure();  // public weather data only
  HTTPClient http;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 4) + "&longitude=" + String(lon, 4) +
               "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=4";
#if USE_FAHRENHEIT
  url += "&temperature_unit=fahrenheit&wind_speed_unit=mph";
#endif
  http.begin(client, url);
  http.setTimeout(10000);
  int wcode = http.GET();
  Serial.printf("wx http=%d\n", wcode);
  if (wcode != 200) { http.end(); return false; }
  JsonDocument d;
  DeserializationError e = deserializeJson(d, http.getString());
  http.end();
  if (e) { Serial.println("wx parse fail"); return false; }
  utcOffset = d["utc_offset_seconds"] | 0;
  JsonObject c = d["current"];
  wx.temp = c["temperature_2m"]; wx.feels = c["apparent_temperature"];
  wx.hum = c["relative_humidity_2m"]; wx.code = c["weather_code"];
  wx.wind = c["wind_speed_10m"]; wx.isDay = c["is_day"] | 1;
  for (int i = 0; i < 3; i++) {
    wx.hi[i] = d["daily"]["temperature_2m_max"][i + 1];
    wx.lo[i] = d["daily"]["temperature_2m_min"][i + 1];
    wx.dcode[i] = d["daily"]["weather_code"][i + 1];
    const char *ds = d["daily"]["time"][i + 1] | "";  // YYYY-MM-DD
    struct tm tmv = {};
    int y, m, dd;
    if (sscanf(ds, "%d-%d-%d", &y, &m, &dd) == 3) {
      tmv.tm_year = y - 1900; tmv.tm_mon = m - 1; tmv.tm_mday = dd; tmv.tm_hour = 12;
      mktime(&tmv);
      char b[8]; strftime(b, sizeof b, "%a", &tmv);
      wx.dayName[i] = b;
    }
  }
  wx.ok = true;
  configTime(utcOffset, 0, "pool.ntp.org", "time.google.com");
  return true;
}

// ---- stock indices (Yahoo Finance chart endpoint, no API key) ----
struct Index { const char *name; const char *sym; bool ok; float price, prev; long rs, re; };  // rs/re: regular session start/end (epoch)
Index idx[3] = {{"S&P 500", "%5EGSPC", false, 0, 0, 0, 0}, {"DAX", "%5EGDAXI", false, 0, 0, 0, 0}, {"NASDAQ", "%5EIXIC", false, 0, 0, 0, 0}};
unsigned long lastMarket = 0;
const unsigned long MARKET_EVERY = 5UL * 60UL * 1000UL;
bool marketTried = false;

bool getMarkets() {
  bool any = false;
  JsonDocument filter;
  filter["chart"]["result"][0]["meta"]["regularMarketPrice"] = true;
  filter["chart"]["result"][0]["meta"]["chartPreviousClose"] = true;
  filter["chart"]["result"][0]["meta"]["currentTradingPeriod"]["regular"] = true;
  for (auto &q : idx) {
    WiFiClientSecure client;
    client.setInsecure();  // public market data only
    HTTPClient http;
    http.begin(client, String("https://query1.finance.yahoo.com/v8/finance/chart/") + q.sym + "?interval=1d&range=1d");
    http.addHeader("User-Agent", "Mozilla/5.0");
    http.setTimeout(8000);
    int code = http.GET();
    Serial.printf("mkt %s http=%d\n", q.name, code);
    if (code == 200) {
      JsonDocument d;
      DeserializationError e = deserializeJson(d, http.getString(), DeserializationOption::Filter(filter));
      JsonObject m = d["chart"]["result"][0]["meta"];
      if (!e && !m["regularMarketPrice"].isNull()) {
        q.price = m["regularMarketPrice"];
        q.prev = m["chartPreviousClose"] | 0.0f;
        q.rs = m["currentTradingPeriod"]["regular"]["start"] | 0L;
        q.re = m["currentTradingPeriod"]["regular"]["end"] | 0L;
        q.ok = true;
        any = true;
      }
    }
    http.end();
  }
  return any;
}

void drawStatic() {
  gfx->fillScreen(C_BG);
  for (auto &s : cache) s = "";
  gfx->drawFastHLine(10, 150, W - 20, C_DIM);
  gfx->drawFastHLine(10, 232, W - 20, C_DIM);
}

void drawClock() {
  struct tm t;
  if (!getLocalTime(&t, 50)) return;
  char b[16];
  strftime(b, sizeof b, USE_24H ? "%H:%M" : "%I:%M", &t);
  row(2, 18, 48, b, 5, C_TIME);
  snprintf(b, sizeof b, "%02d", t.tm_sec);
  row(3, 68, 20, b, 2, C_ACC);
  strftime(b, sizeof b, "%a %d %b", &t);
  row(4, 96, 20, b, 2, C_DIM);
  row(5, 124, 16, city, 1, C_DIM);
}

void drawWeather() {
  if (!wx.ok) { row(6, 160, 20, "Loading weather", 1, C_DIM); return; }
  const char *u = USE_FAHRENHEIT ? "F" : "C";
  row(6, 156, 44, String((int)lroundf(wx.temp)) + u, 5, C_TEMP);
  row(7, 202, 16, wmo(wx.code), 2 > 0 ? 1 : 1, C_TIME);
  row(8, 216, 14, "Feels " + String((int)lroundf(wx.feels)) + "  Hum " + String(wx.hum) + "%  " + String((int)lroundf(wx.wind)) + (USE_FAHRENHEIT ? "mph" : "km/h"), 1, C_DIM);
  for (int i = 0; i < 3; i++) {
    String s = wx.dayName[i] + " " + String((int)lroundf(wx.hi[i])) + "/" + String((int)lroundf(wx.lo[i])) + " " + wmo(wx.dcode[i]);
    if (s.length() > 28) s = s.substring(0, 28);
    row(9 + i, 240 + i * 24, 20, s, 1, i == 0 ? C_TIME : C_DIM);
  }
}

// Market is closed when the current time is outside the exchange's regular session
// (needs the NTP clock; with no valid time we assume open).
bool marketClosed(long rs, long re) {  // plain types: Arduino auto-prototypes come before struct definitions
  time_t now = time(nullptr);
  if (now < 1700000000 || rs == 0 || re == 0) return false;
  return now < rs || now > re;
}

String fmtPrice(float v) {  // 25136.27 -> "25,136.27"
  char b[16];
  snprintf(b, sizeof b, "%.2f", v);
  String s = b;
  int dot = s.indexOf('.');
  for (int i = dot - 3; i > 0; i -= 3) s = s.substring(0, i) + "," + s.substring(i);
  return s;
}

void drawMarkets() {
  row(26, 6, 20, "MARKETS", 2, C_ACC);
  for (int i = 0; i < 3; i++) {
    int y = 40 + i * 92;
    Index &q = idx[i];
    if (i > 0) { if (cache[27 + i * 4] != "line") { cache[27 + i * 4] = "line"; gfx->drawFastHLine(10, y - 4, W - 20, C_DIM); } }
    row(28 + i * 4, y, 18, q.name, 2, C_TIME);
    if (!q.ok) { row(29 + i * 4, y + 24, 44, marketTried ? "no data" : "loading", 2, C_DIM); row(30 + i * 4, y + 66, 18, "", 2, C_DIM); continue; }
    String p = fmtPrice(q.price);
    row(29 + i * 4, y + 22, 28, p, fitSize(p, 3), C_TEMP);
    float chg = q.price - q.prev, pct = q.prev > 0 ? chg / q.prev * 100.0f : 0;
    char b[24];
    snprintf(b, sizeof b, "%c%.2f%%", chg >= 0 ? '+' : '-', fabsf(pct));
    bool closed = marketClosed(q.rs, q.re);
    String c = b;
    if (closed) c += " closed";
    row(30 + i * 4, y + 56, 20, c, 2, closed ? C_CLOSED : (chg >= 0 ? C_UP : C_WARN));
  }
}

// ---- temperature detail: tap the temperature to see temp, feels-like and forecast in big text ----
bool showDetail = false;

int fitSize(const String &s, int maxSize) {  // largest text size (<= maxSize) that fits the screen width
  int size = maxSize;
  while (size > 1 && (int)s.length() * 6 * size > W - 6) size--;
  return size;
}

void drawDetail() {
  if (!wx.ok) return;
  const char *u = USE_FAHRENHEIT ? "F" : "C";
  String t = String((int)lroundf(wx.temp)) + u;
  row(12, 6, 66, t, fitSize(t, 8), C_TEMP);
  String c = wmo(wx.code);
  row(13, 76, 22, c, fitSize(c, 2), C_TIME);
  String f = "Feels " + String((int)lroundf(wx.feels)) + u;
  row(14, 102, 28, f, fitSize(f, 3), C_ACC);
  for (int i = 0; i < 3; i++) {
    String d = wx.dayName[i] + " " + String((int)lroundf(wx.hi[i])) + "/" + String((int)lroundf(wx.lo[i]));
    String k = wmo(wx.dcode[i]);
    int y = 142 + i * 58;
    row(20 + i * 2, y, 28, d, fitSize(d, 3), i == 0 ? C_TIME : C_DIM);
    row(21 + i * 2, y + 28, 20, k, fitSize(k, 2), C_DIM);
  }
}

void setDetail(bool on) {
  showDetail = on;
  gfx->fillScreen(C_BG);
  for (auto &s : cache) s = "";
}

// ---- Claude Code usage (pushed to the board over Wi-Fi: POST /usage?token=...) ----
WebServer server(80);
bool serverStarted = false;
struct Usage {
  bool ok = false;
  String plan;
  int n = 0;
  String label[3];
  int pct[3];
  time_t reset[3];
  int ctx = -1;
  time_t pushed = 0;
} us;

void handleUsage() {
  if (server.arg("token") != USAGE_TOKEN) { server.send(403, "text/plain", "forbidden"); return; }
  JsonDocument d;
  if (deserializeJson(d, server.arg("plain"))) { server.send(400, "text/plain", "bad json"); return; }
  us.plan = String((const char *)(d["plan"] | ""));
  JsonArray w = d["w"];
  us.n = min(3, (int)w.size());
  for (int i = 0; i < us.n; i++) {
    us.label[i] = String((const char *)(w[i]["l"] | ""));
    us.pct[i] = constrain((int)(w[i]["p"] | 0), 0, 100);
    us.reset[i] = (time_t)(w[i]["r"].as<long long>());
  }
  us.ctx = d["ctx"] | -1;
  us.pushed = time(nullptr);
  us.ok = true;
  server.send(200, "text/plain", "ok");
}

String fmtDur(long s) {
  if (s <= 0) return "now";
  long h = s / 3600, m = (s % 3600) / 60;
  if (h >= 24) return String(h / 24) + "d " + String(h % 24) + "h";
  if (h > 0) return String(h) + "h " + String(m) + "m";
  return String(m) + "m";
}

void bar(int slot, int y, int pct) {
  String key = String(pct);
  if (cache[slot] == key) return;
  cache[slot] = key;
  uint16_t col = pct < 60 ? gfx->color565(70, 210, 120) : (pct < 85 ? C_TEMP : C_WARN);
  gfx->drawRoundRect(10, y, 152, 10, 3, C_DIM);
  gfx->fillRect(12, y + 2, 148, 6, C_BG);
  gfx->fillRect(12, y + 2, 148 * pct / 100, 6, col);
}

void drawUsage() {
  row(16, 6, 16, String("CLAUDE CODE") + (us.plan.length() ? " - " + us.plan : ""), 1, C_ACC);
  if (!us.ok) {
    row(17, 120, 20, "No usage yet", 2, C_TIME);
    row(18, 150, 14, "waiting for a push", 1, C_DIM);
    row(19, 170, 14, WiFi.localIP().toString(), 1, C_DIM);
    return;
  }
  time_t now = time(nullptr);
  for (int i = 0; i < us.n; i++) {
    int y = 34 + i * 68;
    row(20 + i * 3, y, 22, us.label[i] + " " + String(us.pct[i]) + "%", 2, C_TIME);
    bar(21 + i * 3, y + 25, us.pct[i]);
    row(22 + i * 3, y + 39, 14, "resets in " + fmtDur((long)(us.reset[i] - now)), 1, C_DIM);
  }
  if (us.ctx >= 0) {
    row(29, 238, 22, "Context " + String(us.ctx) + "%", 2, C_TIME);
    bar(30, 263, us.ctx);
  }
  row(31, 292, 14, "updated " + fmtDur((long)(now - us.pushed)) + " ago", 1, C_DIM);
}

// ---- companion face ----
enum Mode { M_INFO, M_MARKET, M_FACE, M_USAGE, M_HELMET };
const int N_MODES = 5;
const unsigned long ROTATE_EVERY = 5000;  // clock/weather <-> markets
unsigned long lastRotate = 0;
Mode mode = M_INFO;
unsigned long nextBlink = 0, nextGlance = 0;
int eyeDx = 0;  // horizontal glance offset
const int EYE_W = 44, EYE_H = 64, EYE_CY = 115;
const int EYE_X[2] = {46, 126};

void clearEye(int i) {
  gfx->fillRect(EYE_X[i] - EYE_W / 2 - 14, EYE_CY - EYE_H / 2 - 4, EYE_W + 28, EYE_H + 8, C_BG);
}

void drawEyes(int h) {
  uint16_t col = gfx->color565(80, 220, 255);
  for (int i = 0; i < 2; i++) {
    clearEye(i);
    int x = EYE_X[i] - EYE_W / 2 + eyeDx;
    gfx->fillRoundRect(x, EYE_CY - h / 2, EYE_W, h, min(14, h / 2), col);
  }
}

// --- tap reactions ---
void reactWink() {
  uint16_t col = gfx->color565(80, 220, 255);
  clearEye(0);
  gfx->fillRoundRect(EYE_X[0] - EYE_W / 2, EYE_CY - 2, EYE_W, 5, 2, col);
  delay(600);
  drawEyes(EYE_H);
}

void drawHeart(int cx, int cy, uint16_t col) {
  gfx->fillCircle(cx - 10, cy - 8, 12, col);
  gfx->fillCircle(cx + 10, cy - 8, 12, col);
  gfx->fillTriangle(cx - 21, cy - 2, cx + 21, cy - 2, cx, cy + 24, col);
}
void reactHearts() {
  uint16_t pink = gfx->color565(255, 90, 140);
  for (int i = 0; i < 2; i++) { clearEye(i); drawHeart(EYE_X[i], EYE_CY, pink); }
  delay(1300);
  drawEyes(EYE_H);
}

void reactSquint() {  // happy ^ ^ eyes
  uint16_t col = gfx->color565(80, 220, 255);
  for (int i = 0; i < 2; i++) {
    clearEye(i);
    int cx = EYE_X[i];
    for (int o = -2; o <= 2; o++) {
      gfx->drawLine(cx - 18, EYE_CY + 10 + o, cx, EYE_CY - 10 + o, col);
      gfx->drawLine(cx, EYE_CY - 10 + o, cx + 18, EYE_CY + 10 + o, col);
    }
  }
  delay(1100);
  drawEyes(EYE_H);
}

void react() {
  switch (random(3)) {
    case 0: reactWink(); break;
    case 1: reactHearts(); break;
    default: reactSquint(); break;
  }
  nextBlink = millis() + random(2000, 6000);
}

void drawMouth() {
  uint16_t col = gfx->color565(80, 220, 255);
  const int cx = 86, y0 = 215, hw = 34, depth = 20;
  for (int x = cx - hw; x <= cx + hw; x++) {
    float t = (float)(x - cx) / hw;
    int y = y0 + (int)(depth * (1.0f - t * t));
    gfx->fillCircle(x, y, 2, col);
  }
}

// ---- Star Wars helmets (simple fan-art shapes drawn with primitives) ----
int lastHelmet = -1;
int pickHelmet() {
  int k;
  do { k = random(4); } while (k == lastHelmet);
  lastHelmet = k;
  return k;
}

void drawHelmet(int k) {
  gfx->fillScreen(C_BG);
  for (auto &s : cache) s = "";
  const uint16_t W = gfx->color565(235, 235, 240), K = gfx->color565(0, 0, 0);
  const uint16_t GR = gfx->color565(150, 150, 160);
  const char *name = "";
  if (k == 0) {  // Stormtrooper
    name = "STORMTROOPER";
    gfx->fillRoundRect(38, 40, 96, 150, 44, W);
    gfx->fillRoundRect(52, 160, 68, 50, 14, W);
    gfx->fillRect(44, 98, 84, 3, GR);
    gfx->fillRoundRect(52, 108, 30, 24, 10, K);
    gfx->fillRoundRect(90, 108, 30, 24, 10, K);
    gfx->fillTriangle(86, 138, 79, 158, 93, 158, GR);
    gfx->fillRect(66, 136, 3, 16, K);
    gfx->fillRect(103, 136, 3, 16, K);
    gfx->fillRoundRect(64, 174, 44, 18, 3, K);
    for (int i = 0; i < 5; i++) gfx->drawFastVLine(70 + i * 8, 177, 12, W);
  } else if (k == 1) {  // Darth Vader
    name = "DARTH VADER";
    const uint16_t D = gfx->color565(60, 62, 72), R = gfx->color565(150, 150, 165);
    gfx->fillCircle(86, 98, 54, D);
    gfx->drawCircle(86, 98, 54, R);
    gfx->fillRoundRect(26, 120, 120, 14, 6, gfx->color565(85, 87, 98));
    gfx->fillRoundRect(48, 130, 76, 86, 18, D);
    gfx->fillTriangle(48, 200, 124, 200, 86, 238, D);
    gfx->fillRoundRect(54, 140, 26, 14, 6, K);
    gfx->fillRoundRect(92, 140, 26, 14, 6, K);
    gfx->drawRoundRect(54, 140, 26, 14, 6, R);
    gfx->drawRoundRect(92, 140, 26, 14, 6, R);
    gfx->fillTriangle(86, 160, 78, 178, 94, 178, K);
    gfx->fillRoundRect(62, 188, 48, 24, 4, gfx->color565(25, 25, 30));
    gfx->drawRoundRect(62, 188, 48, 24, 4, R);
    for (int i = 0; i < 5; i++) gfx->drawFastVLine(69 + i * 8, 192, 16, R);
  } else if (k == 2) {  // Boba Fett
    name = "BOBA FETT";
    const uint16_t G = gfx->color565(95, 125, 100), RD = gfx->color565(200, 55, 45);
    gfx->drawLine(136, 112, 152, 56, GR);
    gfx->drawLine(137, 112, 153, 56, GR);
    gfx->fillCircle(153, 54, 4, RD);
    gfx->fillRoundRect(40, 40, 92, 176, 38, G);
    gfx->fillRoundRect(48, 56, 76, 10, 4, RD);
    gfx->fillRoundRect(46, 100, 80, 20, 8, K);
    gfx->fillRoundRect(78, 114, 16, 64, 4, K);
    gfx->fillRect(44, 184, 84, 4, RD);
    for (int i = 0; i < 4; i++) gfx->drawFastHLine(58, 194 + i * 5, 56, K);
  } else {  // Clone trooper
    name = "CLONE TROOPER";
    const uint16_t B = gfx->color565(40, 110, 230);
    gfx->fillRoundRect(38, 40, 96, 170, 44, W);
    gfx->fillRect(80, 44, 12, 52, B);
    gfx->fillRoundRect(46, 106, 80, 24, 10, K);
    gfx->fillRoundRect(78, 124, 16, 44, 6, K);
    gfx->fillRect(44, 150, 10, 30, B);
    gfx->fillRect(118, 150, 10, 30, B);
    gfx->fillRoundRect(66, 180, 40, 14, 3, K);
    for (int i = 0; i < 4; i++) gfx->drawFastVLine(72 + i * 9, 183, 8, W);
  }
  row(17, 270, 22, name, 2, C_ACC);
}

void setMode(int m) {  // int, not Mode: Arduino's auto-prototypes are emitted before the enum
  mode = (Mode)m;
  eyeDx = 0;
  showDetail = false;
  lastRotate = millis();
  if (m == M_INFO) {
    drawStatic();
  } else if (m == M_FACE) {
    gfx->fillScreen(C_BG);
    drawEyes(EYE_H);
    drawMouth();
    nextBlink = millis() + random(2000, 6000);
    nextGlance = millis() + random(3000, 8000);
  } else if (m == M_HELMET) {
    drawHelmet(pickHelmet());
  } else {
    gfx->fillScreen(C_BG);
    for (auto &s : cache) s = "";
  }
}

void blink() {
  static const int seq[] = {50, 32, 14, 4, 14, 32, 50, EYE_H};
  for (int h : seq) { drawEyes(h); delay(22); }
}

void faceTick() {
  unsigned long now = millis();
  if (now >= nextGlance) {  // look left or right for a moment, then back
    eyeDx = random(2) ? 9 : -9;
    drawEyes(EYE_H);
    delay(random(500, 900));
    eyeDx = 0;
    drawEyes(EYE_H);
    nextGlance = millis() + random(4000, 9000);
    return;
  }
  if (now < nextBlink) return;
  blink();
  if (random(100) < 20) { delay(90); blink(); }  // occasional double blink
  nextBlink = millis() + random(2000, 6000);
}

void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());
  board_begin();
  C_BG = gfx->color565(0, 0, 0);
  C_TIME = gfx->color565(255, 255, 255);
  C_DIM = gfx->color565(150, 160, 175);
  C_TEMP = gfx->color565(255, 190, 60);
  C_ACC = gfx->color565(60, 200, 255);
  C_WARN = gfx->color565(255, 80, 80);
  C_UP = gfx->color565(70, 220, 120);
  C_CLOSED = gfx->color565(80, 150, 255);
  gfx->fillScreen(C_BG);

  if (!connectWifi()) {
    if (WiFi.status() != WL_CONNECTED && String(WIFI_SSID) != "YOUR_WIFI_NAME") status("Wi-Fi failed", "retrying...");
    else return;  // unconfigured: stay on the message
  }
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  status("Getting data...");
  Serial.printf("wifi ok ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  getWeather();
  getMarkets();
  marketTried = true;
  lastMarket = millis();
  drawStatic();
  lastWeather = millis();
}

void loop() {
  if (String(WIFI_SSID) == "YOUR_WIFI_NAME") { delay(1000); return; }
  if (WiFi.status() != WL_CONNECTED) {
    wifiMulti.run(8000);  // retries every known network
  }
  if (WiFi.status() == WL_CONNECTED && (millis() - lastWeather > WEATHER_EVERY || (!wx.ok && millis() - lastWeather > 30000))) {
    getWeather();
    lastWeather = millis();
  }
  if (WiFi.status() == WL_CONNECTED && millis() - lastMarket > MARKET_EVERY) {
    getMarkets();
    marketTried = true;
    lastMarket = millis();
  }
  static unsigned long lastDraw = 0;
  if (WiFi.status() == WL_CONNECTED) {
    if (!serverStarted) {
      MDNS.begin("clockbuddy");
      server.on("/usage", HTTP_POST, handleUsage);
      server.begin();
      serverStarted = true;
    }
    server.handleClient();
  }
  int g = board_gesture();
  if (g) lastRotate = millis();  // any touch restarts the 5 s timer
  // auto-rotate between clock/weather and markets (not while the detail view is open or on other screens)
  if ((mode == M_INFO || mode == M_MARKET) && !showDetail && millis() - lastRotate > ROTATE_EVERY) {
    setMode(mode == M_INFO ? M_MARKET : M_INFO);
    lastDraw = 0;
  }
  if (g == 1) { setMode((mode + 1) % N_MODES); lastDraw = 0; }
  else if (g == 2 && mode == M_FACE) react();
  else if (g == 2 && mode == M_HELMET) drawHelmet(pickHelmet());
  else if (g == 2 && mode == M_INFO) {
    int tx, ty;
    board_gesture_point(tx, ty);
    // Temperature sits around y=156..200 on screen; the band is generous (120..205) because the
    // touch panel's vertical orientation relative to the screen is not verified.
    if (showDetail) { setDetail(false); drawStatic(); lastDraw = 0; }
    else if (wx.ok && ty >= 120 && ty <= 205) { setDetail(true); lastDraw = 0; }
  }
  if (mode == M_FACE) {
    faceTick();
  } else if (mode == M_MARKET && millis() - lastDraw > 500) {
    lastDraw = millis();
    drawMarkets();
  } else if (mode == M_USAGE && millis() - lastDraw > 1000) {
    lastDraw = millis();
    drawUsage();
  } else if (mode == M_INFO && millis() - lastDraw > 200) {
    lastDraw = millis();
    if (showDetail) {
      drawDetail();
    } else {
      drawClock();
      drawWeather();
    }
  }
  delay(15);
}
