// IMU RECORDER: record the board's motion sensor to a microSD card.
// For the Waveshare ESP32-S3-Touch-LCD-3.5-C.
//
// - Pick a rate (25, 50, 100 or 200 samples a second) and tap REC. Tap STOP to finish.
// - Each recording is a CSV file in the card's /imu folder, named like
//   rec_0007_100Hz.csv, with one row per sample:
//     time_s, ax_g, ay_g, az_g, gx_dps, gy_dps, gz_dps
//   time_s = seconds since REC; a = acceleration in g (1 g = gravity);
//   g = rotation speed in degrees per second. Axes are the sensor chip's own axes.
// - Ranges: acceleration up to +/-8 g, rotation up to +/-1024 degrees/second.
// - The file is saved every second while recording, so pulling the power loses at
//   most the last second.
// - To read the files: take the card out and put it in your computer.
//
// A separate task reads the sensor on a fixed beat, so drawing the screen never
// changes the timing. Samples queue up in a big buffer and get written to the card
// in batches.

#include <Arduino_GFX_Library.h>
#include "board.h"
#include <TouchDrvFT6X36.hpp>
#include <SensorQMI8658.hpp>
#include <SD_MMC.h>
#include <Preferences.h>
#include "FreeSans9pt7b.h"
#include "FreeSansBold9pt7b.h"
#include "FreeSansBold12pt7b.h"

#define SD_CLK 11
#define SD_CMD 10
#define SD_D0 9
#define LCD_SPI_HZ 80000000

const int SCREEN_W = 320, SCREEN_H = 480;

// ---- Hardware ----
TCA9554 expander(0x20);
XPowersPMU pmu;
TouchDrvFT6X36 touch;
SensorQMI8658 imu;
Preferences prefs;
Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, -1, SPI_SCLK, SPI_MOSI, SPI_MISO);
Arduino_ST7796 *lcd = new Arduino_ST7796(bus, -1, 0, true, SCREEN_W, SCREEN_H);
Arduino_Canvas *cv = new Arduino_Canvas(SCREEN_W, SCREEN_H, lcd, 0, 0, 0);
bool imuOk = false, touchOk = false, screenOk = false;

// ---- Samples ----
struct Sample {
  uint32_t tUs;
  float ax, ay, az, gx, gy, gz;
};
const uint32_t RING = 16384;            // about 80 s of buffer at 200 Hz (in PSRAM)
Sample *ring = nullptr;
volatile uint32_t head = 0, tail = 0;   // the sampler adds at head, the writer takes from tail
volatile uint32_t dropped = 0;          // samples lost because the buffer was full
volatile bool recording = false;
volatile int rateHz = 100;
Sample latest = {};
portMUX_TYPE latestLock = portMUX_INITIALIZER_UNLOCKED;

const int RATES[4] = {25, 50, 100, 200};

// ---- Recording state ----
bool sdOk = false;
uint64_t sdFreeMB = 0, sdTotalMB = 0;
File file;
char fileName[40] = "";
char lastSaved[40] = "";
uint32_t recStartUs = 0, recSamples = 0, lastFlushMs = 0;
uint32_t recStartMs = 0;
uint32_t fileCounter = 1;
const char *message = "";
uint32_t messageUntil = 0;

// ---- Live graph history (one point per screen frame) ----
const int HIST = 150;
float histA[3][HIST], histG[3][HIST];
int histN = 0;

// ============================================================ helpers

static inline uint16_t rgb(int r, int g, int b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void textAt(const GFXfont *f, int x, int y, const char *s, uint16_t color) {
  cv->setFont(f);
  cv->setTextSize(1);
  cv->setTextColor(color);
  cv->setCursor(x, y);
  cv->print(s);
  cv->setFont((const GFXfont *)NULL);
}
int textWidth(const GFXfont *f, const char *s) {
  int16_t x1, y1;
  uint16_t w, h;
  cv->setFont(f);
  cv->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  cv->setFont((const GFXfont *)NULL);
  return w;
}
void small(int x, int y, const char *s, uint16_t color) {
  cv->setFont((const GFXfont *)NULL);
  cv->setTextSize(1);
  cv->setTextColor(color);
  cv->setCursor(x, y);
  cv->print(s);
}
void say(const char *msg) {
  message = msg;
  messageUntil = millis() + 4000;
  Serial.println(msg);
}

// ============================================================ the sampler task

void samplerTask(void *) {
  TickType_t last = xTaskGetTickCount();
  while (true) {
    int hz = rateHz;
    TickType_t period = pdMS_TO_TICKS(1000 / hz);
    if (period < 1) period = 1;
    Sample s;
    s.tUs = (uint32_t)esp_timer_get_time();
    if (imuOk) {
      imu.getAccelerometer(s.ax, s.ay, s.az);
      imu.getGyroscope(s.gx, s.gy, s.gz);
    } else {
      s.ax = s.ay = s.az = s.gx = s.gy = s.gz = 0;
    }
    portENTER_CRITICAL(&latestLock);
    latest = s;
    portEXIT_CRITICAL(&latestLock);
    if (recording && ring) {
      if (head - tail < RING) {
        ring[head % RING] = s;
        head = head + 1;
      } else {
        dropped = dropped + 1;
      }
    }
    vTaskDelayUntil(&last, period);
  }
}

// ============================================================ the card

bool mountCard() {
  if (sdOk) return true;
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (!SD_MMC.begin("/sdcard", true)) return false;   // 1-bit mode
  if (SD_MMC.cardType() == CARD_NONE) { SD_MMC.end(); return false; }
  SD_MMC.mkdir("/imu");
  sdOk = true;
  sdTotalMB = SD_MMC.totalBytes() / (1024 * 1024);
  sdFreeMB = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024);
  return true;
}

void startRecording() {
  if (!imuOk) { say("No motion sensor found."); return; }
  if (!mountCard()) { say("No microSD card. Put one in and tap REC."); return; }
  // Next free file name
  for (int tries = 0; tries < 10000; tries++) {
    snprintf(fileName, sizeof(fileName), "/imu/rec_%04lu_%dHz.csv", (unsigned long)fileCounter, (int)rateHz);
    if (!SD_MMC.exists(fileName)) break;
    fileCounter++;
  }
  file = SD_MMC.open(fileName, FILE_WRITE);
  if (!file) { sdOk = false; say("Couldn't create a file on the card."); return; }
  file.print("time_s,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps\n");
  fileCounter++;
  prefs.putUInt("n", fileCounter);
  tail = head;          // start with an empty buffer
  dropped = 0;
  recSamples = 0;
  recStartUs = (uint32_t)esp_timer_get_time();
  recStartMs = millis();
  lastFlushMs = millis();
  recording = true;
  Serial.printf("Recording to %s at %d Hz\n", fileName, (int)rateHz);
}

// Write everything waiting in the buffer to the file.
void drainToCard() {
  if (!file) return;
  static char buf[8192];
  int n = 0;
  while (tail != head) {
    const Sample &s = ring[tail % RING];
    float t = (uint32_t)(s.tUs - recStartUs) / 1e6f;
    n += snprintf(buf + n, sizeof(buf) - n, "%.4f,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f\n",
                  t, s.ax, s.ay, s.az, s.gx, s.gy, s.gz);
    tail = tail + 1;
    recSamples++;
    if (n > (int)sizeof(buf) - 100) { file.write((const uint8_t *)buf, n); n = 0; }
  }
  if (n) file.write((const uint8_t *)buf, n);
  if (millis() - lastFlushMs > 1000) {   // save to the card every second
    file.flush();
    lastFlushMs = millis();
  }
}

void stopRecording() {
  recording = false;
  delay(20);              // let the sampler finish its current sample
  drainToCard();
  file.close();
  snprintf(lastSaved, sizeof(lastSaved), "%s", fileName + 5);   // without "/imu/"
  sdFreeMB = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024);
  Serial.printf("Saved %s: %lu samples, %lu dropped\n", fileName, (unsigned long)recSamples, (unsigned long)dropped);
  say("Saved!");
}

// ============================================================ drawing

const uint16_t BG = 0x10A3, PANEL = 0x18E5, INK = 0xFFFF, MUTED = 0xAD55;
const uint16_t AXIS_COL[3] = {0xF9A6, 0x5F2B, 0x4D9F};   // x red, y green, z blue

void drawGraph(int y, int h, const char *title, const char *unit, float range, float hist[3][HIST], const float now[3]) {
  cv->fillRoundRect(8, y, 304, h, 10, PANEL);
  textAt(&FreeSansBold9pt7b, 18, y + 20, title, INK);
  char buf[48];
  const char *names[3] = {"x", "y", "z"};
  int x = 140;
  for (int i = 0; i < 3; i++) {
    snprintf(buf, sizeof(buf), "%s %+.2f", names[i], now[i]);
    small(x, y + 10, buf, AXIS_COL[i]);
    x += 58;
  }
  small(276, y + 22, unit, MUTED);
  int gx = 18, gw = 284, gy = y + 30, gh = h - 38;
  int mid = gy + gh / 2;
  cv->drawFastHLine(gx, mid, gw, 0x39C7);
  snprintf(buf, sizeof(buf), "+%g", range);
  small(gx, gy, buf, MUTED);
  snprintf(buf, sizeof(buf), "-%g", range);
  small(gx, gy + gh - 8, buf, MUTED);
  if (histN < 2) return;
  for (int a = 0; a < 3; a++) {
    int px = 0, py = 0;
    for (int i = 0; i < histN; i++) {
      float v = hist[a][(HIST - histN + i) % HIST];
      if (v > range) v = range;
      if (v < -range) v = -range;
      int xx = gx + i * gw / (HIST - 1);
      int yy = mid - (int)(v / range * (gh / 2 - 2));
      if (i) cv->drawLine(px, py, xx, yy, AXIS_COL[a]);
      px = xx; py = yy;
    }
  }
}

const int RATE_Y = 384, REC_Y = 424;

void drawScreen() {
  cv->fillScreen(BG);
  // Header
  textAt(&FreeSansBold12pt7b, 12, 32, "IMU Recorder", INK);
  char buf[64];
  if (sdOk) snprintf(buf, sizeof(buf), "SD: %.1f GB free", sdFreeMB / 1024.0);
  else snprintf(buf, sizeof(buf), "no SD card");
  small(320 - 8 - strlen(buf) * 6, 22, buf, sdOk ? MUTED : rgb(255, 120, 110));

  // Status panel
  cv->fillRoundRect(8, 48, 304, 74, 10, PANEL);
  if (recording) {
    bool blink = (millis() / 500) % 2;
    cv->fillCircle(30, 72, 8, blink ? rgb(240, 60, 60) : rgb(120, 30, 30));
    float secs = (millis() - recStartMs) / 1000.0f;
    snprintf(buf, sizeof(buf), "REC  %d:%04.1f", (int)(secs / 60), fmodf(secs, 60));
    textAt(&FreeSansBold12pt7b, 46, 80, buf, INK);
    snprintf(buf, sizeof(buf), "%s", fileName + 5);
    small(20, 96, buf, MUTED);
    snprintf(buf, sizeof(buf), "%lu samples  |  %d Hz  |  lost %lu", (unsigned long)(recSamples + (head - tail)),
             (int)rateHz, (unsigned long)dropped);
    small(20, 108, buf, dropped ? rgb(255, 150, 120) : MUTED);
  } else {
    textAt(&FreeSansBold12pt7b, 20, 78, "Ready", INK);
    if (millis() < messageUntil) small(20, 96, message, rgb(255, 210, 120));
    else small(20, 96, "Pick a rate, then tap REC.", MUTED);
    if (lastSaved[0]) {
      snprintf(buf, sizeof(buf), "Last: %s (%lu samples)", lastSaved, (unsigned long)recSamples);
      small(20, 108, buf, MUTED);
    }
  }

  // Live graphs
  Sample s;
  portENTER_CRITICAL(&latestLock);
  s = latest;
  portEXIT_CRITICAL(&latestLock);
  float a[3] = {s.ax, s.ay, s.az}, g[3] = {s.gx, s.gy, s.gz};
  drawGraph(130, 120, "Acceleration", "g", 2.0f, histA, a);
  drawGraph(256, 120, "Rotation", "deg/s", 250.0f, histG, g);

  // Rate buttons
  for (int i = 0; i < 4; i++) {
    int x = 8 + i * 77;
    bool on = rateHz == RATES[i];
    uint16_t fill = on ? rgb(70, 130, 220) : PANEL;
    cv->fillRoundRect(x, RATE_Y, 72, 34, 8, recording && !on ? BG : fill);
    snprintf(buf, sizeof(buf), "%d Hz", RATES[i]);
    int w = textWidth(&FreeSans9pt7b, buf);
    textAt(&FreeSans9pt7b, x + 36 - w / 2, RATE_Y + 23, buf, recording && !on ? 0x528A : INK);
  }

  // REC / STOP
  uint16_t recCol = recording ? rgb(80, 80, 96) : rgb(220, 50, 60);
  cv->fillRoundRect(8, REC_Y, 304, 48, 12, recCol);
  if (recording) cv->fillRect(122, REC_Y + 15, 18, 18, INK);
  else cv->fillCircle(130, REC_Y + 24, 10, INK);
  textAt(&FreeSansBold12pt7b, 150, REC_Y + 33, recording ? "STOP" : "REC", INK);
}

// ============================================================ touch

bool wasDown = false;

void handleTouch() {
  if (!touchOk) return;
  int16_t xs[1], ys[1];
  bool down = touch.getPoint(xs, ys, 1) > 0;
  if (down && !wasDown) {
    int x = xs[0], y = ys[0];
    if (y >= REC_Y && y <= REC_Y + 48) {
      if (recording) stopRecording();
      else startRecording();
    } else if (y >= RATE_Y && y <= RATE_Y + 34 && !recording) {
      int i = (x - 8) / 77;
      if (i >= 0 && i < 4) { rateHz = RATES[i]; prefs.putInt("rate", rateHz); }
    }
  }
  wasDown = down;
}

// ============================================================ setup & loop

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  BoardStatus st = boardPowerUp(expander, pmu);

  bool lcdOk = lcd->begin(LCD_SPI_HZ, SPI_MODE0);   // mode 0: see bloop_room
  screenOk = lcdOk && cv->begin(GFX_SKIP_OUTPUT_BEGIN);

  touchOk = touch.begin(Wire, FT6X36_SLAVE_ADDRESS);
  imuOk = imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS);
  if (imuOk) {
    // The sensor runs faster than we read it, with its low-pass filter on, so
    // every sample we take is fresh and smooth.
    imu.configAccelerometer(SensorQMI8658::ACC_RANGE_8G, SensorQMI8658::ACC_ODR_500Hz, SensorQMI8658::LPF_MODE_3);
    imu.configGyroscope(SensorQMI8658::GYR_RANGE_1024DPS, SensorQMI8658::GYR_ODR_448_4Hz, SensorQMI8658::LPF_MODE_3);
    imu.enableAccelerometer();
    imu.enableGyroscope();
  }

  prefs.begin("imurec", false);
  fileCounter = prefs.getUInt("n", 1);
  rateHz = prefs.getInt("rate", 100);

  ring = (Sample *)ps_malloc(RING * sizeof(Sample));
  if (!ring) say("Not enough memory for the sample buffer.");
  mountCard();

  Serial.printf("[boot] IMU recorder | PSRAM %s | motion sensor %s | touch %s | SD %s | power chip %s\n",
                st.psram ? "found" : "NOT FOUND", imuOk ? "ok" : "MISSING", touchOk ? "ok" : "MISSING",
                sdOk ? "ok" : "none", st.pmu ? "ok" : "MISSING");

  // The sampler gets its own high-priority task on the other core.
  xTaskCreatePinnedToCore(samplerTask, "imu", 4096, nullptr, configMAX_PRIORITIES - 2, nullptr, 0);
}

uint32_t lastDraw = 0;

void loop() {
  if (recording) drainToCard();
  handleTouch();

  if (screenOk && millis() - lastDraw >= 66) {   // about 15 frames a second
    lastDraw = millis();
    Sample s;
    portENTER_CRITICAL(&latestLock);
    s = latest;
    portEXIT_CRITICAL(&latestLock);
    int i = histN < HIST ? histN : 0;
    if (histN >= HIST) {   // scroll
      for (int a = 0; a < 3; a++) {
        memmove(histA[a], histA[a] + 1, sizeof(float) * (HIST - 1));
        memmove(histG[a], histG[a] + 1, sizeof(float) * (HIST - 1));
      }
      i = HIST - 1;
    } else {
      histN++;
    }
    histA[0][i] = s.ax; histA[1][i] = s.ay; histA[2][i] = s.az;
    histG[0][i] = s.gx; histG[1][i] = s.gy; histG[2][i] = s.gz;
    drawScreen();
    cv->flush();
    static bool backlightOn = false;
    if (!backlightOn) { boardBacklight(true); backlightOn = true; }
  }
  delay(2);
}
