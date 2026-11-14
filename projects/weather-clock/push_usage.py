#!/usr/bin/env python3
"""Push Claude Code usage numbers to the ESP32 clock (shown on its usage screen).

Example:
  python push_usage.py --plan Pro --ctx 15 \
      --w "5-hour|7|2026-11-15T01:09:59Z" --w "Weekly|2|2026-11-14T21:59:59Z" --w "Fable wk|0|2026-11-14T22:00:00Z"
Reads the token from secrets.h. Tries clockbuddy.local, then --host.
"""
import argparse, json, re, sys, urllib.request, urllib.parse
from datetime import datetime, timezone
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("--plan", default="")
ap.add_argument("--ctx", type=int, default=-1, help="session context percent")
ap.add_argument("--w", action="append", default=[], help="label|percent|reset ISO-8601 UTC (up to 3)")
ap.add_argument("--host", default="")
a = ap.parse_args()

token = re.search(r'USAGE_TOKEN\s+"([^"]+)"', (Path(__file__).parent / "secrets.h").read_text()).group(1)
windows = []
for item in a.w[:3]:
    label, pct, reset = item.split("|")
    epoch = int(datetime.fromisoformat(reset.replace("Z", "+00:00")).astimezone(timezone.utc).timestamp())
    windows.append({"l": label, "p": int(pct), "r": epoch})
body = json.dumps({"plan": a.plan, "w": windows, "ctx": a.ctx}).encode()

for host in [h for h in ("clockbuddy.local", a.host) if h]:
    try:
        url = f"http://{host}/usage?" + urllib.parse.urlencode({"token": token})
        req = urllib.request.Request(url, data=body, method="POST", headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=8) as r:
            print(f"pushed to {host}: {r.status} {r.read().decode()}")
            sys.exit(0)
    except Exception as e:
        print(f"{host}: {e}")
sys.exit("could not reach the clock")
