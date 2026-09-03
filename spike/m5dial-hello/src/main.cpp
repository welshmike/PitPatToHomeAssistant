// M5Dial hello-world spike for PaceKeeper hardware bring-up.
// Verifies: encoder, touch, BtnA (WAKE), buzzer and USB CDC serial.
// Build only in this repo -- flashing is done by hand on the real device.

#include <M5Dial.h>

namespace {

constexpr uint32_t kFrameIntervalMs = 50;   // 20 Hz redraw
constexpr uint32_t kSerialIntervalMs = 500; // 2 Hz logging

M5Canvas canvas(&M5Dial.Display);

uint32_t lastFrameMs = 0;
uint32_t lastSerialMs = 0;

long encoderCount = 0;
int32_t touchX = -1;
int32_t touchY = -1;
bool touched = false;
bool btnPressed = false;
uint32_t clickCount = 0;

void drawFrame() {
  canvas.fillSprite(TFT_BLACK);

  canvas.setTextDatum(middle_center);

  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  canvas.setTextSize(1);
  canvas.drawString("ENCODER", 120, 60, &fonts::Font2);

  canvas.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
  canvas.drawString(String(encoderCount), 120, 105, &fonts::Font7);

  canvas.setTextColor(TFT_CYAN, TFT_BLACK);
  if (touched) {
    canvas.drawString("touch " + String(touchX) + "," + String(touchY), 120, 155,
                      &fonts::Font2);
  } else {
    canvas.drawString("touch --", 120, 155, &fonts::Font2);
  }

  canvas.setTextColor(btnPressed ? TFT_ORANGE : TFT_DARKGREY, TFT_BLACK);
  canvas.drawString("BtnA " + String(btnPressed ? "DOWN" : "up") + "  clicks " +
                        String(clickCount),
                    120, 180, &fonts::Font2);

  canvas.pushSprite(0, 0);
}

void logSerial() {
  Serial.printf("enc=%ld touch=%d(%ld,%ld) btnA=%d clicks=%lu\n", encoderCount,
                touched ? 1 : 0, (long)touchX, (long)touchY, btnPressed ? 1 : 0,
                (unsigned long)clickCount);
}

} // namespace

void setup() {
  auto cfg = M5.config();
  M5Dial.begin(cfg, true, false);

  Serial.begin(115200);

  M5Dial.Display.setTextDatum(middle_center);
  M5Dial.Display.fillScreen(TFT_BLACK);

  canvas.setColorDepth(16);
  canvas.createSprite(240, 240);
  canvas.setTextDatum(middle_center);

  M5Dial.Speaker.tone(2000, 80);
  Serial.println("M5Dial hello spike ready");
}

void loop() {
  M5Dial.update();

  encoderCount = M5Dial.Encoder.read();

  auto touch = M5Dial.Touch.getDetail();
  touched = touch.isPressed();
  if (touched) {
    touchX = touch.x;
    touchY = touch.y;
  }

  btnPressed = M5Dial.BtnA.isPressed();
  if (M5Dial.BtnA.wasClicked()) {
    ++clickCount;
    M5Dial.Speaker.tone(2000, 50);
    Serial.println("BtnA click");
  }

  const uint32_t now = millis();
  if (now - lastFrameMs >= kFrameIntervalMs) {
    lastFrameMs = now;
    drawFrame();
  }
  if (now - lastSerialMs >= kSerialIntervalMs) {
    lastSerialMs = now;
    logSerial();
  }
}
