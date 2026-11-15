// heart - a beating heart (65 bpm). Touch it: it swells past the screen edges,
// the whole screen turns red, then it shrinks back over 4 seconds in total.
#include <C6Board.h>
#include <math.h>

const int W = 172, H = 320;
const int CX = W / 2, CY = 120;          // heart centre
const float BPM = 65.0f;
const float PERIOD_MS = 60000.0f / BPM;  // ~923 ms

const float R_REST = 36;                 // resting circle radius
const float R_FULL = 260;                // big enough to cover the whole screen
const uint32_t GROW = 1400, HOLD = 800, SHRINK = 1800;  // 4000 ms total

Arduino_Canvas *canvas;                  // off-screen buffer: no flicker
uint16_t RED, BG;
bool expanding = false;
uint32_t t0 = 0;

// "lub-dub": two quick pulses per beat, 0..1
float beat(float p) {
  float a = (p - 70) / 65.0f, b = (p - 300) / 80.0f;
  return expf(-a * a) + 0.55f * expf(-b * b);
}

float ease(float x) { return x * x * (3 - 2 * x); }

void drawHeart(float R, uint16_t col) {
  float cx = CX, cy = CY;
  canvas->fillCircle(cx - 0.95f * R, cy, R, col);
  canvas->fillCircle(cx + 0.95f * R, cy, R, col);
  canvas->fillTriangle(cx - 1.93f * R, cy + 0.25f * R, cx + 1.93f * R, cy + 0.25f * R,
                       cx, cy + 2.75f * R, col);
}

void setup() {
  board_begin();
  RED = gfx->color565(230, 20, 50);
  BG = RGB565_BLACK;
  canvas = new Arduino_Canvas(W, H, gfx);
  canvas->begin(GFX_SKIP_OUTPUT_BEGIN);
}

void loop() {
  uint32_t now = millis();
  int tx, ty;
  if (!expanding && board_touch(tx, ty)) {
    expanding = true;
    t0 = now;
  }

  canvas->fillScreen(BG);

  if (expanding) {
    uint32_t t = now - t0;
    float e;                                   // 0 = resting size, 1 = fills screen
    if (t < GROW) e = ease(t / (float)GROW);
    else if (t < GROW + HOLD) e = 1;
    else if (t < GROW + HOLD + SHRINK) e = 1 - ease((t - GROW - HOLD) / (float)SHRINK);
    else { expanding = false; e = 0; }
    if (e >= 1) {
      canvas->fillScreen(RED);
    } else {
      float R = R_REST * powf(R_FULL / R_REST, e);  // grows exponentially: feels steady
      drawHeart(R, RED);
    }
  }
  if (!expanding) {
    float p = fmodf((float)now, PERIOD_MS);
    float R = R_REST * (1.0f + 0.14f * beat(p));
    drawHeart(R, RED);
    // small highlight for a bit of depth
    canvas->fillCircle(CX - 0.95f * R - 0.35f * R, CY - 0.4f * R, R * 0.16f, canvas->color565(255, 150, 160));
  }
  canvas->flush();
}
