#pragma once
// tools/flash.py creates secrets.h from this file the first time you flash (secrets.h is git-ignored).
#define WIFI_SSID "YOUR_WIFI_NAME"          // 2.4 GHz network (the C6 cannot join 5 GHz-only)
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

#define USE_24H 1          // 1 = 24-hour clock, 0 = 12-hour
#define USE_FAHRENHEIT 0   // 0 = Celsius / km/h, 1 = Fahrenheit / mph

// Needed by push_usage.py to send Claude Code usage to the board. flash.py generates a random one.
#define USAGE_TOKEN "__GENERATE__"

// Optional: skip IP geolocation and pin a location (uncomment all three).
// #define LAT_OVERRIDE 52.52
// #define LON_OVERRIDE 13.405
// #define CITY_OVERRIDE "Berlin"
