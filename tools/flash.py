#!/usr/bin/env python3
"""ESP32-C6 lab helper for the Waveshare ESP32-C6-Touch-LCD-1.47.

  python tools/flash.py list                      list projects
  python tools/flash.py setup [project]           install the ESP32 core + libraries (once)
  python tools/flash.py flash <project> [-p COM5] build and upload (auto-detects the port)
  python tools/flash.py build <project>           compile only
  python tools/flash.py monitor [-p COM5]         serial monitor (115200)
  python tools/flash.py new <name>                scaffold a new project from the starter template

Needs arduino-cli on PATH (or in tools/bin/). Standard library only.
"""
import argparse
import json
import os
import re
import secrets
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROJECTS = ROOT / "projects"
LIBS = ROOT / "libraries"
CORE = "esp32:esp32"
BASE_FQBN = "esp32:esp32:esp32c6"
# USB-CDC console on the native USB port, 8 MB flash, big app partition (no OTA).
DEFAULT_OPTS = "CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=huge_app"
ESPRESSIF_VID = "0x303a"


def find_cli():
    local = ROOT / "tools" / "bin" / ("arduino-cli.exe" if os.name == "nt" else "arduino-cli")
    found = shutil.which("arduino-cli") or (str(local) if local.exists() else None)
    if not found:
        sys.exit("arduino-cli not found. Install it (Windows: winget install ArduinoSA.CLI, "
                 "macOS: brew install arduino-cli) or place the binary in tools/bin/.")
    return found


def run(args, capture=False):
    print("+", " ".join(str(a) for a in args), flush=True)
    r = subprocess.run([str(a) for a in args], capture_output=capture, text=True)
    if r.returncode != 0:
        if capture and r.stderr:
            print(r.stderr, file=sys.stderr)
        sys.exit(r.returncode)
    return r


def project_dir(name):
    d = PROJECTS / name
    if not (d / f"{name}.ino").exists():
        sys.exit(f"No project '{name}' (expected projects/{name}/{name}.ino). Try: flash.py list")
    return d


def meta(d):
    f = d / "project.json"
    return json.loads(f.read_text()) if f.exists() else {}


def detect_port(cli):
    out = run([cli, "board", "list", "--format", "json"], capture=True).stdout
    data = json.loads(out or "{}")
    entries = data.get("detected_ports", data) if isinstance(data, dict) else data
    ports = []
    for e in entries or []:
        p = e.get("port", {})
        if p.get("protocol", "serial") != "serial":
            continue
        ports.append((p.get("address"), str(p.get("properties", {}).get("vid", "")).lower()))
    esp = [a for a, vid in ports if vid == ESPRESSIF_VID]
    if len(esp) == 1:
        return esp[0]
    if not esp and len(ports) == 1:
        return ports[0][0]
    sys.exit(f"Could not pick a port automatically (found: {[a for a, _ in ports] or 'none'}). Use -p PORT.")


def ensure_secrets(d):
    example, real = d / "secrets.example.h", d / "secrets.h"
    if not example.exists():
        return
    if not real.exists():
        real.write_text(example.read_text().replace("__GENERATE__", secrets.token_hex(12)))
        sys.exit(f"Created {real.relative_to(ROOT)} from the example. Edit it (Wi-Fi etc.), then run again.")
    if "YOUR_WIFI_NAME" in real.read_text():
        sys.exit(f"Edit {real.relative_to(ROOT)} and set your Wi-Fi name/password first.")


def compile_args(cli, d, upload_port=None):
    opts = meta(d).get("fqbn_options", DEFAULT_OPTS)
    args = [cli, "compile", "--fqbn", f"{BASE_FQBN}:{opts}", "--libraries", LIBS, d]
    if upload_port:
        args[2:2] = ["--upload", "-p", upload_port]
    return args


def cmd_list(_):
    for d in sorted(p for p in PROJECTS.iterdir() if (p / f"{p.name}.ino").exists()):
        print(f"{d.name:20} {meta(d).get('description', '')}")


def cmd_setup(a):
    cli = find_cli()
    run([cli, "core", "update-index"])
    run([cli, "core", "install", CORE])
    dirs = [project_dir(a.project)] if a.project else [p for p in PROJECTS.iterdir() if p.is_dir()]
    libs = sorted({lib for d in dirs for lib in meta(d).get("libraries", [])})
    for lib in libs:
        run([cli, "lib", "install", lib])
    if (ROOT / ".git").exists():  # enable the hook that blocks committing secrets
        subprocess.run(["git", "-C", str(ROOT), "config", "core.hooksPath", "tools/hooks"])
    print("Setup done.")


def cmd_flash(a):
    cli, d = find_cli(), project_dir(a.project)
    ensure_secrets(d)
    port = a.port or detect_port(cli)
    run(compile_args(cli, d, upload_port=port))


def cmd_build(a):
    cli, d = find_cli(), project_dir(a.project)
    ensure_secrets(d)
    run(compile_args(cli, d))


def cmd_monitor(a):
    cli = find_cli()
    run([cli, "monitor", "-p", a.port or detect_port(cli), "-c", "baudrate=115200"])


TEMPLATE = '''// {name} - Waveshare ESP32-C6-Touch-LCD-1.47
#include <C6Board.h>

void setup() {{
  board_begin();  // screen + backlight + touch; draw with `gfx`
  gfx->setTextSize(3);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(10, 140);
  gfx->print("{name}");
}}

void loop() {{
  int g = board_gesture();  // BOARD_SWIPE or BOARD_TAP, once per finger lift
  if (g) {{
    gfx->fillScreen(RGB565_BLACK);
    gfx->setCursor(10, 140);
    gfx->print(g == BOARD_SWIPE ? "swipe!" : "tap!");
  }}
  delay(15);
}}
'''


def cmd_new(a):
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", a.name):
        sys.exit("Project names: lowercase letters, digits and dashes (e.g. lava-lamp).")
    d = PROJECTS / a.name
    if d.exists():
        sys.exit(f"projects/{a.name} already exists.")
    d.mkdir(parents=True)
    (d / f"{a.name}.ino").write_text(TEMPLATE.format(name=a.name))
    (d / "project.json").write_text(json.dumps(
        {"description": "TODO: one line about this project", "libraries": ["GFX Library for Arduino"]}, indent=2) + "\n")
    (d / "README.md").write_text(f"# {a.name}\n\nTODO: what it does and how to use it.\n\n"
                                 f"Flash: `python tools/flash.py flash {a.name}`\n")
    print(f"Created projects/{a.name}. Flash it with: python tools/flash.py flash {a.name}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list").set_defaults(fn=cmd_list)
    s = sub.add_parser("setup"); s.add_argument("project", nargs="?"); s.set_defaults(fn=cmd_setup)
    for name, fn in (("flash", cmd_flash), ("build", cmd_build)):
        s = sub.add_parser(name); s.add_argument("project"); s.add_argument("-p", "--port"); s.set_defaults(fn=fn)
    s = sub.add_parser("monitor"); s.add_argument("-p", "--port"); s.set_defaults(fn=cmd_monitor)
    s = sub.add_parser("new"); s.add_argument("name"); s.set_defaults(fn=cmd_new)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
