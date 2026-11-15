# esp32-c6-lab

A home lab of small, fun projects for the **Waveshare ESP32-C6-Touch-LCD-1.47** (172x320 touch screen,
Wi-Fi 6, USB-C). Each project is an Arduino sketch you can flash with one command.

## Quick start

1. Install [arduino-cli](https://arduino.github.io/arduino-cli/) and Python 3
   (Windows: `winget install ArduinoSA.CLI`; macOS: `brew install arduino-cli`).
2. One-time setup (installs the ESP32 core and the libraries the projects need, and turns on the
   commit hook that blocks leaked secrets):
   ```
   python tools/flash.py setup
   ```
3. Plug the board in over USB-C and flash a project:
   ```
   python tools/flash.py flash hello-touch
   ```
   The port is detected automatically; add `-p COM5` to choose one.

## Commands

| Command | What it does |
| --- | --- |
| `python tools/flash.py list` | List projects |
| `python tools/flash.py new <name>` | Create a project from the starter template |
| `python tools/flash.py flash <name>` | Build and upload |
| `python tools/flash.py build <name>` | Compile only |
| `python tools/flash.py monitor` | Serial monitor (115200 baud) |

## Projects

| Project | What it is |
| --- | --- |
| [hello-touch](projects/hello-touch) | Starter template and screen/touch smoke test |
| [weather-clock](projects/weather-clock) | Companion that rotates with weather and markets, tilt-aware face, plane spotter, Claude Code usage, Star Wars helmets |
| [heart](projects/heart) | Beating heart at 65 bpm; touch and it fills the screen red, then returns (4 s) |

## Add a project

```
python tools/flash.py new lava-lamp
python tools/flash.py flash lava-lamp
```

A project is a folder `projects/<name>/` with `<name>.ino` and an optional `project.json`
(`description`, `libraries` to install, `fqbn_options` to override build options). Start every sketch with
`#include <C6Board.h>` and `board_begin();` to get the screen (`gfx`), backlight and touch. Swipes and taps
come from `board_gesture()`.

## Secrets

Anything private (Wi-Fi name and password, tokens) goes in a per-project `secrets.h`, which is **git-ignored**.
Commit a `secrets.example.h` with placeholders instead; `flash.py` copies it to `secrets.h` the first
time you flash and asks you to fill it in. A pre-commit hook (`tools/hooks`, enabled by `flash.py setup`)
refuses to commit any `secrets.h` or a file that looks like it contains a real password or token.
If you clone this repo fresh, run `flash.py setup` once so the hook is active.

## Layout

```
libraries/C6Board/   shared board support: screen, backlight, touch, swipe/tap
projects/            one folder per project
tools/flash.py       build / flash / monitor / scaffold
tools/hooks/         git hooks (secret guard)
docs/board.md        pins, screen and touch notes, gotchas
```

See [docs/board.md](docs/board.md) for hardware notes.

## License

[MIT](LICENSE)
