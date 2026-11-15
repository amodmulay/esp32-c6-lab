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
String cache[64];
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

// Known networks (from secrets.h). A network is only offered when it is in range.
struct Known { const char *ssid; const char *pass; };
const Known KNOWN[] = {
  {WIFI_SSID, WIFI_PASSWORD},
#ifdef WIFI_SSID2
  {WIFI_SSID2, WIFI_PASSWORD2},
#endif
#ifdef WIFI_SSID3
  {WIFI_SSID3, WIFI_PASSWORD3},
#endif
};
const int N_KNOWN = sizeof(KNOWN) / sizeof(KNOWN[0]);

// At every boot, list the known networks that are in range and wait for the user to choose.
// TAP moves the highlight to the next network, SWIPE connects to the highlighted one. Nothing
// connects until you swipe. (Tap/swipe need no knowledge of the touch panel's orientation.)
// `mask` bit i = KNOWN[i] is in range. Returns the index into KNOWN.
int chooseWifi(int mask) {
  int map[4], cnt = 0;
  for (int i = 0; i < N_KNOWN; i++) if (mask & (1 << i)) map[cnt++] = i;
  int sel = 0, shown = -1;
  for (auto &c : cache) c = "";
  gfx->fillScreen(C_BG);
  row(40, 8, 24, "Choose Wi-Fi", 2, C_TIME);
  row(41, 36, 14, "tap = next   swipe = connect", 1, C_DIM);
  while (true) {
    int g = board_gesture();
    if (g == BOARD_SWIPE) return map[sel];
    if (g == BOARD_TAP && cnt > 1) sel = (sel + 1) % cnt;
    if (sel != shown) {
      shown = sel;
      for (int i = 0; i < cnt; i++) {
        int y = 60 + i * 84;
        bool on = i == sel;
        gfx->fillRoundRect(8, y, W - 16, 72, 10, on ? C_ACC : C_BG);
        gfx->drawRoundRect(8, y, W - 16, 72, 10, on ? C_ACC : C_DIM);
        String n = KNOWN[map[i]].ssid;
        int sz = fitSize(n, 2);
        gfx->setTextSize(sz);
        gfx->setTextColor(on ? C_BG : C_TIME, on ? C_ACC : C_BG);
        gfx->setCursor(max(10, (W - (int)n.length() * 6 * sz) / 2), y + (72 - 8 * sz) / 2);
        gfx->print(n);
      }
    }
    delay(15);
  }
}

WiFiMulti *wifiMulti = nullptr;  // joins the strongest known network: home Wi-Fi, or the phone hotspot as a fallback

bool connectWifi() {
  if (String(WIFI_SSID).length() == 0 || String(WIFI_SSID) == "YOUR_WIFI_NAME") {
    status("Set Wi-Fi", "edit secrets.h");
    return false;
  }
  status("Wi-Fi...", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  static bool handlerSet = false;  // connectWifi() runs again after a lost connection
  if (!handlerSet) { handlerSet = true;
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    Serial.printf("wifi disconnect reason=%d (201=AP not found, 2/15/202=auth/password, 204=handshake timeout)\n",
                  info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  }
  delete wifiMulti;  // fresh list each time (this function can run again after a lost connection)
  wifiMulti = new WiFiMulti();
  int found = WiFi.scanNetworks();
  int mask = 0;
  for (int i = 0; i < found; i++)
    for (int k = 0; k < N_KNOWN; k++)
      if (WiFi.SSID(i) == KNOWN[k].ssid) mask |= 1 << k;
  Serial.printf("known networks in range (bitmask): %d\n", mask);
  if (mask) {
    int pick = chooseWifi(mask);
    Serial.printf("user chose %s\n", KNOWN[pick].ssid);
    status("Wi-Fi...", KNOWN[pick].ssid);
    wifiMulti->addAP(KNOWN[pick].ssid, KNOWN[pick].pass);
  } else {
    status("No known Wi-Fi", "searching...");
    for (int k = 0; k < N_KNOWN; k++) wifiMulti->addAP(KNOWN[k].ssid, KNOWN[k].pass);
  }
  wifiMulti->run(20000);
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

// ---- plane spotter: aircraft around you from the community ADS-B feed adsb.lol (no API key) ----
struct Plane { char call[9]; char type[5]; int alt; int gs; float trk, dst, dir; };  // dst in nautical miles, dir = bearing from you
Plane planes[8];
int nPlanes = 0;
bool planesOk = false;
unsigned long lastPlanes = 0;
bool planesFetched = false;
const float PL_RADIUS = 25;  // nautical miles (~46 km)

bool getPlanes() {
  if (!haveLoc) return false;
  JsonDocument filter;
  JsonObject f = filter["ac"].add<JsonObject>();
  f["flight"] = true; f["alt_baro"] = true; f["gs"] = true; f["track"] = true; f["dst"] = true; f["dir"] = true; f["t"] = true;
  WiFiClientSecure client;
  client.setInsecure();  // public aircraft data only
  HTTPClient http;
  http.begin(client, String("https://api.adsb.lol/v2/point/") + String(lat, 3) + "/" + String(lon, 3) + "/" + String((int)PL_RADIUS));
  http.setTimeout(10000);
  int code = http.GET();
  Serial.printf("planes http=%d\n", code);
  if (code != 200) { http.end(); return false; }
  JsonDocument d;
  String body = http.getString();  // handles chunked transfer encoding
  http.end();
  DeserializationError e = deserializeJson(d, body, DeserializationOption::Filter(filter));
  body = "";
  if (e) { Serial.println("planes parse fail"); return false; }
  nPlanes = 0;
  for (JsonObject a : d["ac"].as<JsonArray>()) {
    if (a["dst"].isNull()) continue;
    Plane p;
    String fl = String((const char *)(a["flight"] | ""));
    fl.trim();
    String ty = String((const char *)(a["t"] | ""));
    strlcpy(p.call, fl.length() ? fl.c_str() : "no-id", sizeof p.call);
    strlcpy(p.type, ty.c_str(), sizeof p.type);
    JsonVariant alt = a["alt_baro"];
    p.alt = alt.is<int>() ? alt.as<int>() : 0;  // "ground" -> 0
    p.gs = a["gs"] | 0;
    p.trk = a["track"] | 0.0f;
    p.dst = a["dst"] | 0.0f;
    p.dir = a["dir"] | 0.0f;
    int pos = nPlanes;  // keep the 8 nearest, sorted by distance
    while (pos > 0 && planes[pos - 1].dst > p.dst) pos--;
    if (pos >= 8) continue;
    int last = nPlanes < 8 ? nPlanes : 7;
    for (int i = last; i > pos; i--) planes[i] = planes[i - 1];
    planes[pos] = p;
    if (nPlanes < 8) nPlanes++;
  }
  planesOk = true;
  return true;
}

void drawPlanes() {
  // The radar fills the whole screen (outer ring = PL_RADIUS, north up, you at the centre);
  // text sits in the free corners above and below the circle.
  const int cx = 86, cy = 160, R = 84;
  String head = !planesOk ? (planesFetched ? "no data" : "looking up...")
                          : (nPlanes ? String(nPlanes) + " nearby" : "none in range");
  row(48, 6, 20, "PLANES", 2, C_ACC);
  row(49, 30, 12, head, 1, C_DIM);
  row(60, 44, 12, String((int)(PL_RADIUS * 1.852f)) + " km radius", 1, C_DIM);
  gfx->fillRect(0, 70, W, 180, C_BG);
  gfx->drawCircle(cx, cy, R, C_DIM);
  gfx->drawCircle(cx, cy, R * 2 / 3, C_DIM);
  gfx->drawCircle(cx, cy, R / 3, C_DIM);
  gfx->drawFastHLine(cx - R, cy, 2 * R, C_DIM);
  gfx->drawFastVLine(cx, cy - R, 2 * R, C_DIM);
  gfx->setTextSize(1);
  gfx->setTextColor(C_DIM, C_BG);
  gfx->setCursor(cx + 3, cy - R + 2);
  gfx->print("N");
  gfx->fillCircle(cx, cy, 2, C_TIME);
  for (int i = nPlanes - 1; i >= 0; i--) {  // nearest drawn last, on top
    float r = min(planes[i].dst / PL_RADIUS, 1.0f) * R;
    float a = planes[i].dir * DEG_TO_RAD, h = planes[i].trk * DEG_TO_RAD;
    int px = cx + (int)(r * sinf(a)), py = cy - (int)(r * cosf(a));
    gfx->drawLine(px, py, px + (int)(12 * sinf(h)), py - (int)(12 * cosf(h)), C_TIME);
    gfx->fillCircle(px, py, i == 0 ? 5 : 3, i == 0 ? C_TEMP : C_ACC);
  }
  for (int i = 0; i < 2; i++) {  // the two nearest, below the radar
    int y = 254 + i * 33;
    if (i < nPlanes) {
      Plane &p = planes[i];
      row(50 + i * 2, y, 18, String(p.call) + " " + p.type, 2, i == 0 ? C_TEMP : C_TIME);
      String info = String(p.alt) + "ft  " + String(p.gs) + "kt  " + String((int)lroundf(p.dst * 1.852f)) + "km";
      row(51 + i * 2, y + 18, 12, info, 1, C_DIM);
    } else {
      row(50 + i * 2, y, 18, "", 2, C_DIM);
      row(51 + i * 2, y + 18, 12, "", 1, C_DIM);
    }
  }
}

// ---- closest-aircraft detail view: airline, type and a dot-matrix picture rotated to its heading ----
struct Pair { const char *code; const char *name; };
const Pair AIRLINES[] = {
  {"DLH", "Lufthansa"}, {"EWG", "Eurowings"}, {"CFG", "Condor"}, {"BAW", "British Airways"}, {"RYR", "Ryanair"},
  {"EZY", "easyJet"}, {"EZS", "easyJet Switzerland"}, {"AFR", "Air France"}, {"KLM", "KLM"}, {"SWR", "Swiss"},
  {"AUA", "Austrian"}, {"IBE", "Iberia"}, {"VLG", "Vueling"}, {"SAS", "SAS"}, {"FIN", "Finnair"}, {"LOT", "LOT Polish"},
  {"TAP", "TAP Portugal"}, {"ITY", "ITA Airways"}, {"BEL", "Brussels Airlines"}, {"EIN", "Aer Lingus"},
  {"NAX", "Norwegian"}, {"NOZ", "Norwegian"}, {"WZZ", "Wizz Air"}, {"TUI", "TUI fly"}, {"TOM", "TUI Airways"},
  {"DLA", "Air Dolomiti"}, {"BCS", "European Air Transport"}, {"GEC", "Lufthansa Cargo"}, {"CLX", "Cargolux"},
  {"UAE", "Emirates"}, {"ETD", "Etihad"}, {"QTR", "Qatar Airways"}, {"THY", "Turkish Airlines"}, {"PGT", "Pegasus"},
  {"AEE", "Aegean"}, {"ELY", "El Al"}, {"SVA", "Saudia"}, {"MSR", "EgyptAir"}, {"ETH", "Ethiopian"}, {"SAA", "South African"},
  {"UAL", "United"}, {"DAL", "Delta"}, {"AAL", "American"}, {"SWA", "Southwest"}, {"JBU", "JetBlue"}, {"ACA", "Air Canada"},
  {"ANA", "ANA"}, {"JAL", "Japan Airlines"}, {"SIA", "Singapore Airlines"}, {"CPA", "Cathay Pacific"}, {"QFA", "Qantas"},
  {"KAL", "Korean Air"}, {"AAR", "Asiana"}, {"THA", "Thai Airways"}, {"MAS", "Malaysia Airlines"}, {"AIC", "Air India"},
  {"CCA", "Air China"}, {"CSN", "China Southern"}, {"CES", "China Eastern"}, {"FDX", "FedEx"}, {"UPS", "UPS"},
  {"DHK", "DHL"}, {"DHX", "DHL"}, {"NJE", "NetJets Europe"}, {"EJU", "easyJet Europe"}, {"TVF", "Transavia France"},
  {"TRA", "Transavia"}, {"BEE", "Flybe"}, {"LGL", "Luxair"}, {"AZA", "Alitalia"}, {"SXS", "SunExpress"},
  {"OAW", "Helvetic"}, {"EDW", "Edelweiss"}, {"CAI", "Corendon"}, {"GWI", "Germanwings"}, {"RCH", "US Air Force"},
};

String airlineName(const char *call) {
  String c = call;
  if (c.length() >= 4 && isUpperCase(c[0]) && isUpperCase(c[1]) && isUpperCase(c[2]) && isDigit(c[3])) {
    for (const Pair &a : AIRLINES)
      if (c.startsWith(a.code)) return a.name;
    return String("Airline ") + c.substring(0, 3);
  }
  return "Private / other";
}

const Pair TYPES[] = {
  {"A20N", "A320neo"}, {"A21N", "A321neo"}, {"A19N", "A319neo"}, {"A318", "A318"}, {"A319", "A319"}, {"A320", "A320"},
  {"A321", "A321"}, {"A332", "A330-200"}, {"A333", "A330-300"}, {"A338", "A330-800"}, {"A339", "A330-900"},
  {"A359", "A350-900"}, {"A35K", "A350-1000"}, {"A388", "A380-800"}, {"A343", "A340-300"}, {"A346", "A340-600"},
  {"B737", "737-700"}, {"B738", "737-800"}, {"B739", "737-900"}, {"B38M", "737 MAX 8"}, {"B39M", "737 MAX 9"},
  {"B744", "747-400"}, {"B748", "747-8"}, {"B752", "757-200"}, {"B763", "767-300"}, {"B772", "777-200"},
  {"B77W", "777-300ER"}, {"B788", "787-8"}, {"B789", "787-9"}, {"B78X", "787-10"}, {"E170", "Embraer 170"},
  {"E190", "Embraer 190"}, {"E195", "Embraer 195"}, {"E75L", "Embraer 175"}, {"E290", "Embraer E190-E2"},
  {"CRJ7", "CRJ-700"}, {"CRJ9", "CRJ-900"}, {"BCS1", "A220-100"}, {"BCS3", "A220-300"}, {"DH8D", "Dash 8 Q400"},
  {"AT75", "ATR 72-500"}, {"AT76", "ATR 72-600"}, {"AT72", "ATR 72"}, {"C172", "Cessna 172"}, {"PC12", "Pilatus PC-12"},
  {"CL60", "Challenger 600"}, {"GLF4", "Gulfstream IV"}, {"GLF5", "Gulfstream V"}, {"C56X", "Citation Excel"},
};

String typeName(const char *t) {
  for (const Pair &x : TYPES)
    if (strcmp(x.code, t) == 0) return x.name;
  return t[0] ? String(t) : String("Unknown type");
}

// 0 narrow-body jet, 1 wide-body twin, 2 four-engine, 3 regional jet, 4 propeller, 5 helicopter
int planeCategory(const char *t) {
  String y = t;
  y.toUpperCase();
  if (y == "A388" || y == "B744" || y == "B748" || y == "A343" || y == "A345" || y == "A346" || y == "B742" || y == "B743") return 2;
  const char *heli[] = {"EC", "AS3", "AS5", "A109", "A139", "A169", "A189", "R22", "R44", "R66", "B06", "B407", "B412", "B429", "S76", "S92"};
  for (const char *h : heli) if (y.startsWith(h)) return 5;
  if (y.length() >= 2 && y[0] == 'H' && isDigit(y[1])) return 5;
  if (y == "A310" || y.startsWith("A30") || y.startsWith("A33") || y.startsWith("A35") || y.startsWith("B76") || y.startsWith("B77") ||
      y.startsWith("B78")) return 1;
  const char *reg[] = {"E1", "E2", "E7", "CRJ", "BCS", "RJ", "F70", "F100"};
  for (const char *h : reg) if (y.startsWith(h)) return 3;
  const char *prop[] = {"AT", "DH8", "DHC", "SF3", "C1", "C2", "PA", "SR2", "BE", "PC", "TB", "DA", "P28", "M20", "F50", "JS", "B190"};
  for (const char *h : prop) if (y.startsWith(h)) return 4;
  return 0;
}

// Is the point (u = to the right of the nose direction, v = along it; about -1..1) part of the aircraft silhouette?
bool inPlane(float u, float v, int cat) {
  float au = fabsf(u);
  if (cat == 5) {  // helicopter: rotor ring, body, tail boom, tail rotor
    float rr = sqrtf(u * u + (v - 0.1f) * (v - 0.1f));
    if (fabsf(rr - 0.82f) < 0.045f) return true;
    float ex = u / 0.15f, ey = (v - 0.1f) / 0.32f;
    if (ex * ex + ey * ey < 1) return true;
    if (au < 0.04f && v > -0.85f && v < -0.15f) return true;
    return au < 0.13f && v < -0.78f && v > -0.9f;
  }
  float fw = cat == 1 ? 0.10f : (cat == 2 ? 0.11f : (cat == 4 ? 0.075f : 0.085f));
  float span = cat == 1 ? 0.95f : (cat == 2 ? 0.98f : (cat == 3 ? 0.68f : (cat == 4 ? 0.86f : 0.82f)));
  float w = fw;
  if (v > 0.6f) w = fw * (0.98f - v) / 0.38f;
  else if (v < -0.68f) w = fw * (0.5f + 0.5f * (v + 0.96f) / 0.28f);
  if (v > -0.96f && v < 0.98f && au < w) return true;
  if (cat == 4) {  // straight wing, small tailplane, engine nacelles with propeller discs
    if (au > 0.05f && au < span && v > 0.0f && v < 0.2f) return true;
    if (au < 0.28f && v > -0.9f && v < -0.76f) return true;
    return fabsf(au - 0.3f) < 0.05f && v > 0.18f && v < 0.3f;
  }
  float sweep = cat == 3 ? 0.5f : 0.62f;
  float le = 0.18f - (au - 0.08f) * sweep;
  float chord = (cat == 3 ? 0.26f : 0.34f) - (au - 0.08f) * 0.28f;
  if (au > 0.06f && au < span && v < le && v > le - chord) return true;
  float tle = -0.62f - (au - 0.06f) * 0.5f;
  if (au > 0.06f && au < (cat == 3 ? 0.26f : 0.34f) && v < tle && v > tle - 0.14f) return true;
  if (cat == 3) return fabsf(au - 0.16f) < 0.045f && v > -0.62f && v < -0.42f;  // rear-mounted engines
  if (fabsf(au - 0.3f) < 0.045f && v > -0.06f && v < 0.12f) return true;
  if (cat == 2 && fabsf(au - 0.52f) < 0.045f && v > -0.14f && v < 0.04f) return true;
  return false;
}

void drawPlaneImage(float trackDeg, int cat, uint16_t col) {
  const int cx = 86, cy = 150, N = 26, STEP = 5;
  const float S = (N / 2.0f) * STEP;  // 1 unit in pixels
  float th = trackDeg * DEG_TO_RAD, sn = sinf(th), cs = cosf(th);
  uint16_t off = gfx->color565(28, 34, 44);
  gfx->fillRect(cx - 70, cy - 70, 140, 140, C_BG);
  for (int gy = 0; gy < N; gy++)
    for (int gx = 0; gx < N; gx++) {
      float dx = (gx - (N - 1) / 2.0f) * STEP, dy = (gy - (N - 1) / 2.0f) * STEP;
      float u0 = dx / S, v0 = -dy / S;  // screen frame: north up
      float v = u0 * sn + v0 * cs;      // along the nose
      float u = u0 * cs - v0 * sn;      // to the right of the nose
      int px = cx + (int)dx, py = cy + (int)dy;
      if (inPlane(u, v, cat)) gfx->fillCircle(px, py, 2, col);
      else gfx->drawPixel(px, py, off);
    }
}

const char *compass8(float deg) {
  static const char *n[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return n[((int)((deg + 22.5f) / 45.0f)) % 8];
}

int planesView = 0;  // 0 = radar, 1 = closest-aircraft details
unsigned long viewAt = 0;
const unsigned long VIEW_EVERY = 10000;

void drawPlaneDetail() {
  if (!nPlanes) return;
  Plane &p = planes[0];
  String al = airlineName(p.call);
  row(48, 6, 22, al, fitSize(al, 2), C_ACC);
  row(49, 32, 26, p.call, 3, C_TEMP);
  drawPlaneImage(p.trk, planeCategory(p.type), gfx->color565(255, 190, 60));
  String ty = typeName(p.type);
  row(50, 236, 18, ty, fitSize(ty, 2), C_TIME);
  String a = String(p.alt) + "ft " + String(p.gs) + "kt";
  row(51, 258, 18, a, fitSize(a, 2), C_TIME);
  String d = "hdg " + String((int)lroundf(p.trk)) + "   " + String((int)lroundf(p.dst * 1.852f)) + " km " + compass8(p.dir);
  row(52, 280, 14, d, 1, C_DIM);
  row(53, 298, 14, "closest of " + String(nPlanes), 1, C_DIM);
}

void showPlanesView() {  // full redraw of the current view
  if (planesView == 1 && !nPlanes) planesView = 0;
  gfx->fillScreen(C_BG);
  for (auto &c : cache) c = "";
  if (planesView) drawPlaneDetail(); else drawPlanes();
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
enum Mode { M_INFO, M_MARKET, M_FACE, M_USAGE, M_HELMET, M_PLANES };
const int N_MODES = 6;
// Auto-rotation: companion face 15-20 s, then weather 4 s, then markets 4 s, then back to the face.
unsigned long rotateAt = 0;
Mode mode = M_FACE;
unsigned long nextBlink = 0, nextGlance = 0;
int eyeDx = 0;  // horizontal glance offset
int tiltDx = 0, tiltDy = 0;  // eye offset that follows how the board is tilted
const int EYE_W = 44, EYE_H = 64, EYE_CY = 115;
const int EYE_X[2] = {46, 126};

void clearEye(int i) {
  gfx->fillRect(EYE_X[i] - EYE_W / 2 - 26, EYE_CY - EYE_H / 2 - 14, EYE_W + 52, EYE_H + 28, C_BG);
}

void drawEyes(int h) {
  uint16_t col = gfx->color565(80, 220, 255);
  clearEye(0);
  clearEye(1);  // clear both first: the clear areas overlap in the middle
  for (int i = 0; i < 2; i++) {
    int x = EYE_X[i] - EYE_W / 2 + eyeDx + tiltDx;
    gfx->fillRoundRect(x, EYE_CY + tiltDy - h / 2, EYE_W, h, min(14, h / 2), col);
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
  tiltDx = tiltDy = 0;
  showDetail = false;
  rotateAt = millis() + (m == M_FACE ? random(15000, 20001) : 4000);
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
  } else if (m == M_PLANES) {
    gfx->fillScreen(C_BG);
    for (auto &s : cache) s = "";
    planesFetched = false;
    lastPlanes = 0;
    planesView = 0;
    viewAt = millis() + VIEW_EVERY;
    drawPlanes();
  } else {
    gfx->fillScreen(C_BG);
    for (auto &s : cache) s = "";
  }
}

void blink() {
  static const int seq[] = {50, 32, 14, 4, 14, 32, 50, EYE_H};
  for (int h : seq) { drawEyes(h); delay(22); }
}

// ---- tilt awareness (accelerometer: ax>0 = left edge down, ay<0 = standing upright, az<0 = lying face up) ----
bool asleep = false;
unsigned long upsideSince = 0, shakeCool = 0, lastImu = 0;
float shakeScore = 0, pAx = 0, pAy = 0, pAz = 0;

void drawSleep() {
  uint16_t col = gfx->color565(80, 220, 255);
  clearEye(0);
  clearEye(1);
  for (int i = 0; i < 2; i++)
    for (int o = -2; o <= 2; o++) gfx->drawFastHLine(EYE_X[i] - 20, EYE_CY + o, 40, col);
  gfx->setTextSize(3);
  gfx->setTextColor(C_DIM, C_BG);
  gfx->setCursor(128, 40);
  gfx->print("z");
  gfx->setCursor(144, 22);
  gfx->print("Z");
}

void dizzy() {  // eyes wobble in opposite directions for about 1.6 s
  uint16_t col = gfx->color565(80, 220, 255);
  unsigned long t0 = millis();
  while (millis() - t0 < 1600) {
    float ph = (millis() - t0) * 0.02f;
    clearEye(0);
    clearEye(1);
    for (int i = 0; i < 2; i++) {
      float sgn = i ? -1.0f : 1.0f;
      int dx = (int)(8 * cosf(ph * sgn)), dy = (int)(12 * sinf(ph * sgn));
      gfx->fillRoundRect(EYE_X[i] - EYE_W / 2 + dx, EYE_CY - 22 + dy, EYE_W, 44, 14, col);
    }
    delay(40);
  }
  drawEyes(EYE_H);
}

void tiltTick(unsigned long now) {
  if (now - lastImu < 60) return;
  lastImu = now;
  float x, y, z;
  if (!board_imu(x, y, z)) return;
  float d = fabsf(x - pAx) + fabsf(y - pAy) + fabsf(z - pAz);
  pAx = x; pAy = y; pAz = z;
  shakeScore = shakeScore * 0.6f + d;
  // upside down (standing, top edge toward the floor) for a moment: fall asleep; wake when upright again
  if (y > 0.6f) { if (!upsideSince) upsideSince = now; } else upsideSince = 0;
  if (!asleep && upsideSince && now - upsideSince > 700) { asleep = true; drawSleep(); return; }
  if (asleep) {
    if (y < 0.3f) { asleep = false; shakeScore = 0; drawEyes(EYE_H); }
    return;
  }
  if (shakeScore > 3.0f && now > shakeCool) {
    Serial.printf("shake score=%.1f\n", shakeScore);
    dizzy();
    shakeCool = millis() + 4000;
    shakeScore = 0;
    return;
  }
  // eyes look toward the low side: left edge down -> look left; lying face up -> look up at you
  int tx = fabsf(x) < 0.06f ? 0 : constrain((int)(-x * 20), -13, 13);
  int ty = constrain((int)((z + 0.1f) * 9), -8, 8);
  if (abs(tx - tiltDx) >= 2 || abs(ty - tiltDy) >= 2) {
    tiltDx += (tx - tiltDx + (tx > tiltDx ? 1 : -1)) / 2;
    tiltDy += (ty - tiltDy + (ty > tiltDy ? 1 : -1)) / 2;
    drawEyes(EYE_H);
  }
}

void faceTick() {
  unsigned long now = millis();
  tiltTick(now);
  if (asleep) return;
  if (now >= nextGlance && abs(tiltDx) < 4) {  // look left or right for a moment, then back
    eyeDx = random(2) ? 9 : -9;
    drawEyes(EYE_H);
    delay(random(500, 900));
    eyeDx = 0;
    drawEyes(EYE_H);
    nextGlance = millis() + random(4000, 9000);
    return;
  }
  if (now >= nextGlance) nextGlance = now + 3000;  // tilted: skip the glance
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
  lastWeather = millis();
  setMode(M_FACE);  // start with the companion
}

void loop() {
  if (String(WIFI_SSID) == "YOUR_WIFI_NAME") { delay(1000); return; }
  // Connection lost: the ESP32 retries by itself for a while; if it is still down after 15 s,
  // go back to the Wi-Fi chooser so you can pick a network again.
  static unsigned long lostSince = 0;
  if (WiFi.status() == WL_CONNECTED) {
    lostSince = 0;
  } else {
    if (lostSince == 0) lostSince = millis();
    if (millis() - lostSince > 15000) {
      Serial.println("wifi lost -> chooser");
      WiFi.disconnect(true);
      delay(300);
      connectWifi();
      lostSince = 0;
      if (WiFi.status() == WL_CONNECTED) {
        if (serverStarted) { MDNS.end(); MDNS.begin("clockbuddy"); }
        lastWeather = millis() - WEATHER_EVERY - 1;  // refresh data on the new network
        lastMarket = millis() - MARKET_EVERY - 1;
      }
      setMode(mode);  // redraw the current screen
    }
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
  if (g) rotateAt = millis() + (mode == M_FACE ? random(15000, 20001) : 4000);  // any touch restarts the timer
  // auto-rotate: face 15-20 s -> weather 4 s -> markets 4 s -> face (not on the detail view, other screens, or while asleep)
  if ((mode == M_FACE || mode == M_INFO || mode == M_MARKET) && !showDetail && !asleep && millis() > rotateAt) {
    setMode(mode == M_FACE ? M_INFO : (mode == M_INFO ? M_MARKET : M_FACE));
    lastDraw = 0;
  }
  if (g == 1) { setMode((mode + 1) % N_MODES); lastDraw = 0; }
  else if (g == 2 && mode == M_FACE) react();
  else if (g == 2 && mode == M_HELMET) drawHelmet(pickHelmet());
  else if (g == 2 && mode == M_PLANES && nPlanes) { planesView ^= 1; viewAt = millis() + VIEW_EVERY; showPlanesView(); }
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
  } else if (mode == M_PLANES) {
    if (WiFi.status() == WL_CONNECTED && (lastPlanes == 0 || millis() - lastPlanes > 15000)) {
      getPlanes();
      planesFetched = true;
      lastPlanes = millis();
      if (planesView == 1 && nPlanes) drawPlaneDetail(); else showPlanesView();
    }
    if (millis() > viewAt) {  // every 10 s: radar <-> details of the closest aircraft
      viewAt = millis() + VIEW_EVERY;
      if (nPlanes) { planesView ^= 1; showPlanesView(); }
    }
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
