// hello-touch - Waveshare ESP32-C6-Touch-LCD-1.47
#include <C6Board.h>

void setup() {
  board_begin();  // screen + backlight + touch; draw with `gfx`
  gfx->setTextSize(3);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(10, 140);
  gfx->print("hello-touch");
}

void loop() {
  int g = board_gesture();  // BOARD_SWIPE or BOARD_TAP, once per finger lift
  if (g) {
    gfx->fillScreen(RGB565_BLACK);
    gfx->setCursor(10, 140);
    gfx->print(g == BOARD_SWIPE ? "swipe!" : "tap!");
  }
  delay(15);
}
