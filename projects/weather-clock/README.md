# weather-clock

Clock and weather for the ESP32-C6-Touch-LCD-1.47, plus a few fun screens. The board starts on the companion and
**rotates by itself**: companion face for 15-20 seconds, then weather for 4 seconds, then markets for 4 seconds, then
back to the companion. **Swipe** to cycle by hand through all screens (swiping or tapping restarts the timer; the
rotation only runs between those three, not on the other screens):

1. **Clock + weather**: time (NTP), date, city, temperature, conditions, 3-day forecast.
   **Tap the temperature** for a full-screen view of the temperature, feels-like and forecast in large text; tap again to go back.
   (The detail view pauses the rotation.)
2. **Markets**: S&P 500, DAX, NASDAQ with change vs. previous close. Green = up, red = down, blue = exchange closed.
3. **Companion face**: two eyes and a smile that blink at random, glance around, and react when tapped (wink, hearts, happy eyes).
   It is **tilt-aware** (accelerometer): the eyes look toward the low side when you tilt the board and look up at you when it
   lies flat, it gets **dizzy when shaken**, and **falls asleep (closed eyes, Zzz) when stood upside down**.
4. **Claude Code usage**: 5-hour and weekly limits as bars with reset countdowns, plus session context use.
5. **Star Wars helmets**: a random Stormtrooper, Darth Vader, Boba Fett or Clone Trooper drawn from simple shapes. Tap for another.
6. **Plane spotter**: a radar of the aircraft within ~46 km of your location (north up, with each plane's heading) and a list of the
   4 nearest with callsign, type, altitude (ft), speed (kt) and distance. Data comes from the community ADS-B feed
   [adsb.lol](https://adsb.lol) (no key); it refreshes every 15 seconds while the screen is showing.

Location comes from your IP address (or fixed values in `secrets.h`); weather comes from [Open-Meteo](https://open-meteo.com) (no API key).

## Setup

```
python tools/flash.py flash weather-clock
```

The first run creates `projects/weather-clock/secrets.h` from `secrets.example.h` (with a random usage token).
Fill in your Wi-Fi name and password (2.4 GHz), then run the command again. `secrets.h` is git-ignored.
Units (C/F) and 12/24-hour time are in the same file.

## Claude Code usage screen

The usage numbers only exist inside Claude Code, so the board can't fetch them. Instead, a script pushes a
snapshot to the board over your home network (`clockbuddy.local`, authenticated with the token in `secrets.h`):

```
python projects/weather-clock/push_usage.py --plan Pro --ctx 15 \
    --w "5-hour|7|2026-11-15T01:09:59Z" --w "Weekly|2|2026-11-14T21:59:59Z" --w "Fable wk|0|2026-11-14T22:00:00Z"
```

Each `--w` is `label|percent|reset time (UTC, ISO-8601)`; up to three. In Claude Code, ask it to read your usage and push it.
The screen shows how long ago the last push was. If `clockbuddy.local` doesn't resolve, add `--host <board IP>`
(the IP is shown on the usage screen before the first push).

## Wi-Fi choice (home network, phone hotspots)

`secrets.h` can hold up to three known networks (`WIFI_SSID`, `WIFI_SSID2`, `WIFI_SSID3`, each with a password).
At every boot the board scans and shows a **Choose Wi-Fi** screen listing the known networks that are in range.
**Tap** moves the highlight to the next one, **swipe** connects to the highlighted one. It never connects on its own
while that screen is up. If the connection is lost for more than 15 seconds, the board returns to this screen so you can choose again. If none of them is in range it keeps trying all of them. Hotspots must be 2.4 GHz
(iPhone: Personal Hotspot > Maximize Compatibility, and keep that screen open while the board scans). On a
hotspot, weather and market data use mobile data.

## Notes

- Debug output (Wi-Fi status, HTTP codes) goes to the serial monitor: `python tools/flash.py monitor`.
- The tap target for the temperature is a wide band because touch orientation isn't verified; see `docs/board.md`.
