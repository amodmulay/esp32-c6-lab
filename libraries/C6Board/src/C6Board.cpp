#include "C6Board.h"
#include <Wire.h>

static Arduino_DataBus *bus = new Arduino_ESP32SPI(C6_PIN_DC, C6_PIN_CS, C6_PIN_SCK, C6_PIN_MOSI, GFX_NOT_DEFINED);
// Rotation 4 = mirrored X: needed so text reads correctly on this panel.
// IPS flag false = no colour inversion. 34 px column offset centres 172 px in the 240 px controller RAM.
Arduino_GFX *gfx = new Arduino_ST7789(bus, C6_PIN_RST, 4, false, C6_WIDTH, C6_HEIGHT, 34, 0, 34, 0);

static bool touchReady = false;
static uint8_t imuAddr = 0;

static bool imuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(imuAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission(true) == 0;
}

static bool imuRead(uint8_t reg, uint8_t *buf, int n) {
  Wire.beginTransmission(imuAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)imuAddr, n) != n) return false;
  for (int i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

// QMI8658: WHO_AM_I (0x00) = 0x05. CTRL1 0x02 (auto-increment), CTRL2 0x03 (accel: +-4 g, 250 Hz), CTRL7 0x08 (enable accel).
static void imuBegin() {
  const uint8_t addrs[2] = {0x6B, 0x6A};
  for (uint8_t a : addrs) {
    imuAddr = a;
    uint8_t id = 0;
    if (imuRead(0x00, &id, 1) && id == 0x05) {
      imuWrite(0x02, 0x40);
      imuWrite(0x03, 0x15);
      imuWrite(0x08, 0x01);
      delay(20);
      return;
    }
  }
  imuAddr = 0;
}

bool board_imu_ready() { return imuAddr != 0; }

bool board_imu(float &ax, float &ay, float &az) {
  if (!imuAddr) return false;
  uint8_t b[6];
  if (!imuRead(0x35, b, 6)) return false;
  ax = (int16_t)(b[0] | (b[1] << 8)) / 8192.0f;
  ay = (int16_t)(b[2] | (b[3] << 8)) / 8192.0f;
  az = (int16_t)(b[4] | (b[5] << 8)) / 8192.0f;
  return true;
}

void board_begin(bool withTouch) {
  pinMode(C6_PIN_BL, OUTPUT);
  digitalWrite(C6_PIN_BL, HIGH);
  gfx->begin();
  gfx->fillScreen(RGB565_BLACK);
  if (withTouch) {
    pinMode(C6_PIN_TP_RST, OUTPUT);
    digitalWrite(C6_PIN_TP_RST, LOW);
    delay(10);
    digitalWrite(C6_PIN_TP_RST, HIGH);
    delay(60);
    Wire.begin(C6_PIN_TP_SDA, C6_PIN_TP_SCL, 100000);
    touchReady = true;
    imuBegin();
  }
}

void board_backlight(bool on) { digitalWrite(C6_PIN_BL, on ? HIGH : LOW); }

// AXS5106L: read 14 bytes from register 0x01.
//   [1] = touch count, x = ((b[2] & 0xF) << 8) | b[3], y = ((b[4] & 0xF) << 8) | b[5]
bool board_touch(int &x, int &y) {
  if (!touchReady) return false;
  uint8_t b[14] = {0};
  Wire.beginTransmission(0x63);
  Wire.write((uint8_t)0x01);
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom(0x63, 14) != 14) return false;
  for (int i = 0; i < 14; i++) b[i] = Wire.read();
  if (b[1] == 0) return false;
  x = ((b[2] & 0x0F) << 8) | b[3];
  y = ((b[4] & 0x0F) << 8) | b[5];
  return true;
}

static int gestX = 0, gestY = 0;
void board_gesture_point(int &x, int &y) { x = gestX; y = gestY; }

int board_gesture() {
  static bool down = false;
  static int sx, sy, lx, ly;
  static unsigned long t0;
  int x, y;
  if (board_touch(x, y)) {
    if (!down) { down = true; sx = x; sy = y; t0 = millis(); }
    lx = x; ly = y;
    return BOARD_NONE;
  }
  if (down) {
    down = false;
    gestX = lx; gestY = ly;
    if (abs(lx - sx) >= 35 || abs(ly - sy) >= 35) return BOARD_SWIPE;
    if (millis() - t0 < 500) return BOARD_TAP;
  }
  return BOARD_NONE;
}
