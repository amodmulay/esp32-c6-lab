// Waveshare ESP32-C6-Touch-LCD-1.47 board support.
//   Screen: 172x320 IPS, JD9853 controller (driven with the ST7789 driver), SPI.
//   Touch:  AXS5106L on I2C address 0x63.
// Use:  #include <C6Board.h>   then   board_begin();   and draw with `gfx`.
#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

// --- Pins (from the Waveshare wiki main page) ---
#define C6_PIN_SCK 1
#define C6_PIN_MOSI 2
#define C6_PIN_CS 14
#define C6_PIN_DC 15
#define C6_PIN_RST 22
#define C6_PIN_BL 23
#define C6_PIN_TP_SDA 18
#define C6_PIN_TP_SCL 19
#define C6_PIN_TP_RST 20
#define C6_PIN_TP_INT 21

#define C6_WIDTH 172
#define C6_HEIGHT 320

extern Arduino_GFX *gfx;  // the screen; valid after board_begin()

// Initialise screen (cleared to black), backlight on, and touch. Call once in setup().
void board_begin(bool withTouch = true);
void board_backlight(bool on);

// Current touch point. Returns true while a finger is down.
bool board_touch(int &x, int &y);

// Call every loop(). Returns once per finger lift:
//   BOARD_SWIPE (>= 35 px travel), BOARD_TAP (quick, small movement), else BOARD_NONE.
enum { BOARD_NONE = 0, BOARD_SWIPE = 1, BOARD_TAP = 2 };
int board_gesture();
// Where the finger lifted on the most recent gesture (raw touch coordinates, 0..171 x 0..319).
void board_gesture_point(int &x, int &y);

// QMI8658 accelerometer (shared I2C bus). Started by board_begin(true); returns false if not found.
// Values are in g (about +-1 when still), in the sensor's own axes.
bool board_imu_ready();
bool board_imu(float &ax, float &ay, float &az);
