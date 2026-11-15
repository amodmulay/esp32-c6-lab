# weather-clock

Clock and weather for the ESP32-C6-Touch-LCD-1.47, plus a few fun screens. **Swipe** to cycle:

1. **Clock + weather**: time (NTP), date, city, temperature, conditions, 3-day forecast.
   **Tap the temperature** for a full-screen view of the temperature, feels-like and forecast in large text; tap again to go back.
   The clock/weather screen and the **Markets** screen (S&P 500, DAX, NASDAQ with change vs. previous close) **rotate by themselves every 5 seconds**. Any touch restarts the timer; opening the temperature detail view or swiping to another screen stops the rotation.
2. **Companion face**: two eyes and a smile that blink at random, glance around, and react when tapped (wink, hearts, happy eyes).
3. **Claude Code usage**: 5-hour and weekly limits as bars with reset countdowns, plus session context use.
4. **Star Wars helmets**: a random Stormtrooper, Darth Vader, Boba Fett or Clone Trooper drawn from simple shapes. Tap for another.

Index changes are green (up) or red (down); when an exchange is outside its regular trading hours the line turns **blue** and says "closed".

Index quotes come from Yahoo Finance's public chart endpoint (no API key, unofficial, may be delayed) and refresh every 5 minutes. They are informational only.

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

## Phone hotspot fallback

Uncomment `WIFI_SSID2` / `WIFI_PASSWORD2` in `secrets.h` to add a second network, such as your phone's hotspot.
The board joins the strongest known network and retries both if the connection drops. The hotspot must be
2.4 GHz (iPhone: Personal Hotspot > Maximize Compatibility). On a hotspot, weather and market data use mobile data.

## Notes

- Debug output (Wi-Fi status, HTTP codes) goes to the serial monitor: `python tools/flash.py monitor`.
- The tap target for the temperature is a wide band because touch orientation isn't verified; see `docs/board.md`.
