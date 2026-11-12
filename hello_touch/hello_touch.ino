// hello_touch: first test for the Waveshare ESP32-S3-Touch-LCD-3.5-C
//
// 1. Shows a title and color bars, so you can check the screen and its colors.
// 2. Draw on the screen with your finger. The touch position is shown at the top.
// 3. Tap CLEAR at the bottom to wipe the drawing. Tap a color swatch to change pen color.
//
// Pins come from Waveshare's official examples for this board
// (github.com/waveshareteam/ESP32-S3-Touch-LCD-3.5).

#include <Arduino_GFX_Library.h>
#include <TouchDrvFT6X36.hpp>
#include "board.h"   // pins and power-up

#define LCD_CS    -1    // tied low on the board
#define LCD_RST   -1    // reset goes through the TCA9554 IO expander instead

#define LCD_W 320
#define LCD_H 480

TCA9554 expander(0x20);
XPowersPMU pmu;
BoardStatus boardSt;
TouchDrvFT6X36 touch;

Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, SPI_SCLK, SPI_MOSI, SPI_MISO);
Arduino_GFX *gfx = new Arduino_ST7796(bus, LCD_RST, 0 /* rotation */, true /* IPS */, LCD_W, LCD_H);

// ---- Layout ----
const int HEADER_H = 70;            // title + touch readout
const int FOOTER_Y = LCD_H - 60;    // swatches + CLEAR button
const int CANVAS_TOP = HEADER_H;
const int CANVAS_BOTTOM = FOOTER_Y;
const int CLEAR_X = 220, CLEAR_W = 92;

const uint16_t PEN_COLORS[] = {RGB565_WHITE, RGB565_RED, RGB565_GREEN, RGB565_BLUE, RGB565_YELLOW};
const int NUM_COLORS = sizeof(PEN_COLORS) / sizeof(PEN_COLORS[0]);
const int SWATCH = 36, SWATCH_GAP = 6, SWATCH_X0 = 8;

uint16_t penColor = RGB565_WHITE;
bool wasTouching = false;
int16_t lastX, lastY;
uint32_t touchCount = 0;

void drawReadout(const char *text) {
  gfx->fillRect(0, 40, LCD_W, 26, RGB565_BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_LIGHTGREY);
  gfx->setCursor(8, 44);
  gfx->print(text);
}

void drawFooter() {
  gfx->fillRect(0, FOOTER_Y, LCD_W, LCD_H - FOOTER_Y, RGB565_DARKGREY);
  for (int i = 0; i < NUM_COLORS; i++) {
    int x = SWATCH_X0 + i * (SWATCH + SWATCH_GAP);
    gfx->fillRect(x, FOOTER_Y + 12, SWATCH, SWATCH, PEN_COLORS[i]);
    if (PEN_COLORS[i] == penColor) {
      gfx->drawRect(x - 3, FOOTER_Y + 9, SWATCH + 6, SWATCH + 6, RGB565_ORANGE);
      gfx->drawRect(x - 2, FOOTER_Y + 10, SWATCH + 4, SWATCH + 4, RGB565_ORANGE);
    }
  }
  gfx->fillRoundRect(CLEAR_X, FOOTER_Y + 10, CLEAR_W, 40, 8, RGB565_MAROON);
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(CLEAR_X + 16, FOOTER_Y + 23);
  gfx->print("CLEAR");
}

// The canvas starts with color bars so you can check the colors are right.
void drawCanvas() {
  const uint16_t bars[] = {RGB565_RED, RGB565_GREEN, RGB565_BLUE, RGB565_WHITE};
  const char *names[] = {"RED", "GREEN", "BLUE", "WHITE"};
  int barW = LCD_W / 4;
  gfx->fillRect(0, CANVAS_TOP, LCD_W, CANVAS_BOTTOM - CANVAS_TOP, RGB565_BLACK);
  for (int i = 0; i < 4; i++) {
    gfx->fillRect(i * barW, CANVAS_TOP, barW, 30, bars[i]);
    gfx->setTextSize(1);
    gfx->setTextColor(i == 3 ? RGB565_BLACK : RGB565_WHITE);
    gfx->setCursor(i * barW + 6, CANVAS_TOP + 11);
    gfx->print(names[i]);
  }
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_DARKGREY);
  gfx->setCursor(70, (CANVAS_TOP + CANVAS_BOTTOM) / 2 - 8);
  gfx->print("Draw here!");
}

void drawScreen() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextSize(3);
  gfx->setTextColor(RGB565_CYAN);
  gfx->setCursor(8, 8);
  gfx->print("Hello, ESP32!");
  drawReadout("Touch the screen");
  drawCanvas();
  drawFooter();
}

void handleTap(int16_t x, int16_t y) {
  if (y < FOOTER_Y + 8) return;
  if (x >= CLEAR_X && x < CLEAR_X + CLEAR_W) {
    drawCanvas();
    return;
  }
  for (int i = 0; i < NUM_COLORS; i++) {
    int sx = SWATCH_X0 + i * (SWATCH + SWATCH_GAP);
    if (x >= sx && x < sx + SWATCH) {
      penColor = PEN_COLORS[i];
      drawFooter();
      return;
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  boardSt = boardPowerUp(expander, pmu);

  bool screenOk = gfx->begin(40000000);
  drawScreen();
  boardBacklight(true);

  bool touchOk = touch.begin(Wire, FT6X36_SLAVE_ADDRESS);
  if (!touchOk) drawReadout("Touch chip not found!");

  Serial.printf("[boot] hello_touch | reset: %s | PSRAM %s | IO expander %s | power chip %s | screen %s | touch %s\n",
                resetReasonName(boardSt.reason), boardSt.psram ? "found" : "NOT FOUND",
                boardSt.expander ? "ok" : "MISSING", boardSt.pmu ? "ok" : "MISSING",
                screenOk ? "ok" : "FAILED", touchOk ? "ok" : "MISSING");
}

void loop() {
  int16_t xs[1], ys[1];
  bool touching = touch.getPoint(xs, ys, 1) > 0;

  if (touching) {
    int16_t x = constrain(xs[0], 0, LCD_W - 1);
    int16_t y = constrain(ys[0], 0, LCD_H - 1);

    if (!wasTouching) {
      touchCount++;
      Serial.printf("touch #%lu at %d,%d\n", (unsigned long)touchCount, x, y);
      handleTap(x, y);
    }

    char buf[40];
    snprintf(buf, sizeof(buf), "X:%3d  Y:%3d  #%lu", x, y, (unsigned long)touchCount);
    drawReadout(buf);

    // Draw only inside the canvas, joining points so fast strokes stay smooth.
    if (y > CANVAS_TOP + 4 && y < CANVAS_BOTTOM - 4) {
      if (wasTouching && lastY > CANVAS_TOP + 4 && lastY < CANVAS_BOTTOM - 4) {
        for (int d = -1; d <= 1; d++) {
          gfx->drawLine(lastX + d, lastY, x + d, y, penColor);
          gfx->drawLine(lastX, lastY + d, x, y + d, penColor);
        }
      } else {
        gfx->fillCircle(x, y, 2, penColor);
      }
    }
    lastX = x;
    lastY = y;
  }

  wasTouching = touching;
  delay(10);
}
