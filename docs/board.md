# Board notes: Waveshare ESP32-C6-Touch-LCD-1.47

Verified on real hardware unless marked otherwise.

## Pins (from the Waveshare wiki main page)

| Function | GPIO |
| --- | --- |
| LCD SCLK / MOSI | 1 / 2 (shared with the TF card slot) |
| LCD CS / DC / RST | 14 / 15 / 22 |
| LCD backlight | 23 (active high, worked as plain on/off) |
| Touch SDA / SCL | 18 / 19 (shared I2C bus with the QMI8658 IMU) |
| Touch RST / INT | 20 / 21 |
| TF card MISO / CS | 3 / 4 |
| USB D- / D+ | 12 / 13 |
| Boot button | 8 |

The wiki's Arduino example lists GPIO 38/39/45/47, which do not exist on an ESP32-C6. Ignore those.

## Screen

- 172x320, controller JD9853. Arduino_GFX has no JD9853 class, but its **ST7789** driver draws correctly.
- Settings that work: `Arduino_ST7789(bus, RST, rotation 4, ips false, 172, 320, 34, 0, 34, 0)`.
  - **Rotation 4** (mirrored X) is required, or text appears mirrored.
  - **IPS flag false**: `true` inverts the colours.
  - Column offset 34 centres the 172 px panel in the controller's 240 px RAM.

## Touch (AXS5106L, I2C address 0x63)

- Pulse TP_RST low for 10 ms before use. I2C at 100 kHz works.
- Read 14 bytes from register `0x01`: byte 1 is the touch count; `x = ((b2 & 0xF) << 8) | b3`,
  `y = ((b4 & 0xF) << 8) | b5`.
- INT only pulses briefly while a finger moves, so don't wait on it; polling every ~15 ms works fine.
- Not verified: whether touch Y runs top-to-bottom or the reverse relative to the screen.
  The weather clock's tap target is therefore a deliberately wide band.

## Build settings

- ESP32 Arduino core 3.3.x, board `esp32:esp32:esp32c6`.
- `CDCOnBoot=cdc` (serial over the native USB port), `FlashSize=8M`, `PartitionScheme=huge_app`.
  The default partition holds only about 1.3 MB, and Wi-Fi + graphics already filled 92% of it before
  the web server was added. `huge_app` gives 3 MB but removes OTA updates.
- Opening the serial port from a script can reset the board; that is normal.

## Gotchas

- Wi-Fi is 2.4 GHz only.
- Arduino auto-generates function prototypes before your `enum` definitions; a function taking an enum
  parameter then fails with "'Mode' was not declared". Take an `int` instead.
- Don't name a variable `detail`: it collides with a namespace in the C++ library headers.
- After a failed Wi-Fi connect the IP is 0.0.0.0 and every request fails. The weather clock keeps going and
  retries from `loop()`, so the screen sits on "Loading weather" until Wi-Fi works. Check the serial monitor.
