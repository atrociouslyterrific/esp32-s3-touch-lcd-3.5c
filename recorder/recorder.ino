// RECORDER: video from the camera and data from the motion sensor (IMU), recorded
// together onto a microSD card. For the Waveshare ESP32-S3-Touch-LCD-3.5-C.
//
// - The screen shows the camera and a live motion graph. Pick a video size and a
//   motion rate (either can be OFF), then tap REC. Tap STOP to finish.
// - Each recording makes up to three files in the card's /rec folder:
//     rec_0001.avi         video at 20 frames a second (MJPEG; plays in VLC, QuickTime...)
//     rec_0001_imu.csv     time_s, ax_g, ay_g, az_g, gx_dps, gy_dps, gz_dps
//     rec_0001_frames.csv  frame, time_s   (when each video frame was taken)
//   All times are seconds since you tapped REC, on the same clock, so the motion
//   data lines up with the video: frame N was taken at time_s in the frames file.
// - Motion: acceleration in g (1 g = gravity), up to +/-8 g; rotation in degrees per
//   second, up to +/-1024. Axes are the sensor chip's own axes.
// - Files are saved every couple of seconds, so pulling the power loses very little.
// - Longest recording: 60 minutes (or until the card is full).
//
// How it keeps its timing: one task reads the motion sensor on a fixed beat, another
// takes a picture every 50 ms; both put their data in waiting rooms in PSRAM. A third
// task writes everything to the card. A slow moment on the card doesn't lose data
// unless a waiting room fills up (the screen shows how much was lost, if any).

#include <Arduino_GFX_Library.h>
#include "board.h"
#include <TouchDrvFT6X36.hpp>
#include <SensorQMI8658.hpp>
#include <esp_camera.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <JPEGDEC.h>
#include "avi.h"
#include "FreeSans9pt7b.h"
#include "FreeSansBold9pt7b.h"
#include "FreeSansBold12pt7b.h"
#if __has_include("build_info.h")
#include "build_info.h"
#endif
#ifndef FIRMWARE_BUILD
#define FIRMWARE_BUILD "local"
#endif

// ---- Pins (camera pins from Waveshare's board support code) ----
#define SD_CLK 11
#define SD_CMD 10
#define SD_D0 9
#define CAM_XCLK 38
#define CAM_PCLK 41
#define CAM_VSYNC 17
#define CAM_HREF 18
#define CAM_D0 45
#define CAM_D1 47
#define CAM_D2 48
#define CAM_D3 46
#define CAM_D4 42
#define CAM_D5 40
#define CAM_D6 39
#define CAM_D7 21
#define LCD_SPI_HZ 80000000

const int SCREEN_W = 320, SCREEN_H = 480;
const int FPS = 20;
const uint32_t MAX_SECONDS = 60 * 60;

// ---- Hardware ----
TCA9554 expander(0x20);
XPowersPMU pmu;
TouchDrvFT6X36 touch;
SensorQMI8658 imu;
Preferences prefs;
JPEGDEC jpeg;
Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, -1, SPI_SCLK, SPI_MOSI, SPI_MISO);
Arduino_ST7796 *lcd = new Arduino_ST7796(bus, -1, 0, true, SCREEN_W, SCREEN_H);
Arduino_Canvas *cv = new Arduino_Canvas(SCREEN_W, SCREEN_H, lcd, 0, 0, 0);
bool camOk = false, imuOk = false, touchOk = false, screenOk = false, sdOk = false;

// ---- Choices ----
struct Size { framesize_t fs; uint16_t w, h; const char *name; int scale; };
const int NUM_SIZES = 3;
const Size SIZES[NUM_SIZES] = {
  {FRAMESIZE_VGA, 640, 480, "640x480", JPEG_SCALE_HALF},
  {FRAMESIZE_SVGA, 800, 600, "800x600", JPEG_SCALE_QUARTER},
  {FRAMESIZE_HD, 1280, 720, "1280x720", JPEG_SCALE_QUARTER},
};
int sizeIdx = 0;          // NUM_SIZES = video off
const int NUM_RATES = 4;
const int RATES[NUM_RATES] = {25, 50, 100, 200};
int rateIdx = 2;          // NUM_RATES = motion off
bool videoOn() { return sizeIdx < NUM_SIZES; }
bool imuOn() { return rateIdx < NUM_RATES; }

// ---- Video waiting room: frame slots in PSRAM ----
const int SLOTS = 24;                     // about 1.2 s of video (4.6 MB)
const uint32_t SLOT_BYTES = 192 * 1024;   // biggest frame kept (bigger ones are dropped)
uint8_t *slotMem = nullptr;
uint32_t slotLen[SLOTS], slotTimeUs[SLOTS];
QueueHandle_t freeSlots, fullSlots;

// ---- Motion waiting room ----
struct Sample { uint32_t tUs; float ax, ay, az, gx, gy, gz; };
const uint32_t RING = 16384;              // about 80 s at 200 Hz
Sample *ring = nullptr;
volatile uint32_t head = 0, tail = 0;
Sample latest = {};
portMUX_TYPE latestLock = portMUX_INITIALIZER_UNLOCKED;
volatile int imuHz = 100;

// ---- Preview ----
uint8_t *previewBuf = nullptr;
volatile uint32_t previewLen = 0;
volatile bool previewWanted = true;

// ---- Recording state (shared between tasks) ----
volatile bool recording = false;
volatile bool stopAsked = false;
volatile bool writerIdle = true;
volatile uint32_t framesSaved = 0, framesDropped = 0, bytesSaved = 0, samplesSaved = 0, samplesDropped = 0;
volatile bool writeFailed = false;
AviWriter avi;
File imuFile, frameFile;
char baseName[32] = "";
char lastSaved[72] = "";
uint32_t recStartUs = 0, recStartMs = 0;
uint32_t fileCounter = 1;
uint64_t sdFreeMB = 0;
const char *message = "";
uint32_t messageUntil = 0;

static inline uint16_t rgb(int r, int g, int b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
void say(const char *m) { message = m; messageUntil = millis() + 4000; Serial.println(m); }

// ============================================================ camera

bool startCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = CAM_D0; c.pin_d1 = CAM_D1; c.pin_d2 = CAM_D2; c.pin_d3 = CAM_D3;
  c.pin_d4 = CAM_D4; c.pin_d5 = CAM_D5; c.pin_d6 = CAM_D6; c.pin_d7 = CAM_D7;
  c.pin_xclk = CAM_XCLK; c.pin_pclk = CAM_PCLK; c.pin_vsync = CAM_VSYNC; c.pin_href = CAM_HREF;
  c.pin_sccb_sda = -1;          // the camera shares the board's I2C bus (already started)
  c.pin_sccb_scl = -1;
  c.sccb_i2c_port = 0;
  c.pin_pwdn = -1;
  c.pin_reset = -1;
  c.xclk_freq_hz = 20000000;
  c.frame_size = FRAMESIZE_HD;  // biggest first, so the buffers fit any size; then switch down
  c.pixel_format = PIXFORMAT_JPEG;
  c.grab_mode = CAMERA_GRAB_LATEST;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.jpeg_quality = 12;
  c.fb_count = 2;
  if (esp_camera_init(&c) != ESP_OK) return false;
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, 1);         // mounted upside down (as in Waveshare's firmware)
    s->set_framesize(s, SIZES[videoOn() ? sizeIdx : 0].fs);
  }
  return true;
}

// Takes a picture every 50 ms.
void captureTask(void *) {
  TickType_t last = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(1000 / FPS);
  while (true) {
    if (camOk) {
      camera_fb_t *fb = esp_camera_fb_get();
      if (fb) {
        uint32_t t = (uint32_t)esp_timer_get_time();
        if (recording && !stopAsked && videoOn()) {
          int slot;
          if (fb->len + 10 <= SLOT_BYTES && xQueueReceive(freeSlots, &slot, 0) == pdTRUE) {
            memcpy(slotMem + (size_t)slot * SLOT_BYTES + 8, fb->buf, fb->len);   // 8 bytes left for the chunk header
            slotLen[slot] = fb->len;
            slotTimeUs[slot] = t;
            xQueueSend(fullSlots, &slot, 0);
          } else {
            framesDropped = framesDropped + 1;
          }
        }
        if (previewWanted && fb->len <= SLOT_BYTES) {
          memcpy(previewBuf, fb->buf, fb->len);
          previewLen = fb->len;
          previewWanted = false;
        }
        esp_camera_fb_return(fb);
      }
    }
    vTaskDelayUntil(&last, period);
  }
}

// ============================================================ motion sensor

// Reads the motion sensor on a fixed beat (always, so the screen graph is live).
void imuTask(void *) {
  TickType_t last = xTaskGetTickCount();
  while (true) {
    int hz = imuHz;
    TickType_t period = pdMS_TO_TICKS(1000 / hz);
    if (period < 1) period = 1;
    Sample s;
    s.tUs = (uint32_t)esp_timer_get_time();
    imu.getAccelerometer(s.ax, s.ay, s.az);
    imu.getGyroscope(s.gx, s.gy, s.gz);
    portENTER_CRITICAL(&latestLock);
    latest = s;
    portEXIT_CRITICAL(&latestLock);
    if (recording && !stopAsked && imuOn()) {
      if (head - tail < RING) {
        ring[head % RING] = s;
        head = head + 1;
      } else {
        samplesDropped = samplesDropped + 1;
      }
    }
    vTaskDelayUntil(&last, period);
  }
}

// ============================================================ writing to the card

void writeImuSamples() {
  if (!imuFile) return;
  static char buf[8192];
  int n = 0;
  while (tail != head) {
    const Sample &s = ring[tail % RING];
    float t = (int32_t)(s.tUs - recStartUs) / 1e6f;
    n += snprintf(buf + n, sizeof(buf) - n, "%.4f,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f\n",
                  t, s.ax, s.ay, s.az, s.gx, s.gy, s.gz);
    tail = tail + 1;
    samplesSaved = samplesSaved + 1;
    if (n > (int)sizeof(buf) - 100) {
      if (imuFile.write((const uint8_t *)buf, n) != (size_t)n) writeFailed = true;
      n = 0;
    }
  }
  if (n && imuFile.write((const uint8_t *)buf, n) != (size_t)n) writeFailed = true;
}

// The one task that writes to the card: video frames, frame times and motion data.
void writerTask(void *) {
  uint32_t lastFlush = millis();
  while (true) {
    int slot;
    bool got = xQueueReceive(fullSlots, &slot, pdMS_TO_TICKS(20)) == pdTRUE;
    if (!recording) { writerIdle = true; if (got) xQueueSend(freeSlots, &slot, 0); continue; }
    writerIdle = false;
    if (got) {
      if (!writeFailed) {
        if (avi.addFrame(slotMem + (size_t)slot * SLOT_BYTES, slotLen[slot])) {
          char line[32];
          int n = snprintf(line, sizeof(line), "%lu,%.4f\n", (unsigned long)framesSaved,
                           (int32_t)(slotTimeUs[slot] - recStartUs) / 1e6f);
          frameFile.write((const uint8_t *)line, n);
          framesSaved = framesSaved + 1;
          bytesSaved = bytesSaved + slotLen[slot] + 8;
        } else {
          writeFailed = true;   // card full or removed
        }
      }
      xQueueSend(freeSlots, &slot, 0);
    }
    if (imuOn()) writeImuSamples();
    if (millis() - lastFlush > 2000) {   // save to the card every couple of seconds
      if (avi.f) avi.f.flush();
      if (frameFile) frameFile.flush();
      if (imuFile) imuFile.flush();
      lastFlush = millis();
    }
    writerIdle = true;
  }
}

bool mountCard() {
  if (sdOk) return true;
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_HIGHSPEED)) return false;
  if (SD_MMC.cardType() == CARD_NONE) { SD_MMC.end(); return false; }
  SD_MMC.mkdir("/rec");
  sdOk = true;
  sdFreeMB = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024);
  return true;
}

void startRecording() {
  if (!videoOn() && !imuOn()) { say("Video and motion are both OFF."); return; }
  if (videoOn() && !camOk) { say("Camera not found. Set video to OFF to record motion only."); return; }
  if (imuOn() && !imuOk) { say("Motion sensor not found. Set motion to OFF."); return; }
  if (!mountCard()) { say("No microSD card. Put one in and tap REC."); return; }
  char path[48];
  for (int tries = 0; tries < 10000; tries++) {
    snprintf(baseName, sizeof(baseName), "/rec/rec_%04lu", (unsigned long)fileCounter);
    snprintf(path, sizeof(path), "%s.avi", baseName);
    bool taken = SD_MMC.exists(path);
    snprintf(path, sizeof(path), "%s_imu.csv", baseName);
    taken = taken || SD_MMC.exists(path);
    if (!taken) break;
    fileCounter++;
  }
  fileCounter++;
  prefs.putUInt("n", fileCounter);

  if (videoOn()) {
    snprintf(path, sizeof(path), "%s.avi", baseName);
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f || !avi.begin(f, SIZES[sizeIdx].w, SIZES[sizeIdx].h, FPS * MAX_SECONDS + FPS * 10)) {
      if (f) f.close();
      say("Couldn't start the video file.");
      return;
    }
    snprintf(path, sizeof(path), "%s_frames.csv", baseName);
    frameFile = SD_MMC.open(path, FILE_WRITE);
    if (frameFile) frameFile.print("frame,time_s\n");
  }
  if (imuOn()) {
    snprintf(path, sizeof(path), "%s_imu.csv", baseName);
    imuFile = SD_MMC.open(path, FILE_WRITE);
    if (!imuFile) { say("Couldn't create the motion file."); if (videoOn()) { avi.end(0); frameFile.close(); } return; }
    imuFile.print("time_s,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps\n");
  }

  framesSaved = 0; framesDropped = 0; bytesSaved = 0; samplesSaved = 0; samplesDropped = 0;
  writeFailed = false;
  stopAsked = false;
  tail = head;
  recStartUs = (uint32_t)esp_timer_get_time();
  recStartMs = millis();
  recording = true;
  Serial.printf("Recording %s: video %s, motion %s\n", baseName, videoOn() ? SIZES[sizeIdx].name : "off",
                imuOn() ? String(RATES[rateIdx]).c_str() : "off");
}

void stopRecording() {
  stopAsked = true;                          // no new frames or samples
  uint32_t elapsedUs = (uint32_t)esp_timer_get_time() - recStartUs;
  uint32_t t0 = millis();
  while ((uxQueueMessagesWaiting(fullSlots) > 0 || tail != head) && millis() - t0 < 10000) delay(10);   // write what's left
  while (!writerIdle && millis() - t0 < 12000) delay(5);
  recording = false;
  delay(30);
  while (!writerIdle) delay(5);
  if (videoOn()) {
    avi.end(framesSaved ? elapsedUs : 0);   // stamped with the real frame rate
    if (frameFile) frameFile.close();
  }
  if (imuFile) imuFile.close();
  float secs = elapsedUs / 1e6f;
  if (videoOn())
    snprintf(lastSaved, sizeof(lastSaved), "%s: %.0f s, %.1f fps, %lu motion rows", baseName + 5, secs,
             secs > 0 ? framesSaved / secs : 0, (unsigned long)samplesSaved);
  else
    snprintf(lastSaved, sizeof(lastSaved), "%s: %.0f s, %lu motion rows", baseName + 5, secs, (unsigned long)samplesSaved);
  sdFreeMB = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024);
  Serial.printf("Saved %s: %lu frames (%lu lost), %lu motion rows (%lu lost), %.1f s\n", baseName,
                (unsigned long)framesSaved, (unsigned long)framesDropped, (unsigned long)samplesSaved,
                (unsigned long)samplesDropped, secs);
  say(writeFailed ? "Stopped: the card is full or was removed." : "Saved!");
}

// ============================================================ drawing

const uint16_t BG = 0x10A3, PANEL = 0x18E5, INK = 0xFFFF, MUTED = 0xAD55;
const uint16_t AXIS_COL[3] = {0xF9A6, 0x5F2B, 0x4D9F};   // x red, y green, z blue
const int PREV_Y = 46, PREV_H = 240, GRAPH_Y = 290, GRAPH_H = 52, STAT_Y = 346, OPT_Y = 386, REC_Y = 426;
int previewX = 0, previewY = 0;

const int HIST = 150;
float hist[3][HIST];
int histN = 0;

int drawJpegBlock(JPEGDRAW *d) {
  cv->draw16bitRGBBitmap(previewX + d->x, previewY + d->y, d->pPixels, d->iWidth, d->iHeight);
  return 1;
}

void textAt(const GFXfont *f, int x, int y, const char *s, uint16_t color) {
  cv->setFont(f); cv->setTextSize(1); cv->setTextColor(color); cv->setCursor(x, y); cv->print(s);
  cv->setFont((const GFXfont *)NULL);
}
int textWidth(const GFXfont *f, const char *s) {
  int16_t x1, y1; uint16_t w, h;
  cv->setFont(f); cv->getTextBounds(s, 0, 0, &x1, &y1, &w, &h); cv->setFont((const GFXfont *)NULL);
  return w;
}
void small(int x, int y, const char *s, uint16_t color) {
  cv->setFont((const GFXfont *)NULL); cv->setTextSize(1); cv->setTextColor(color); cv->setCursor(x, y); cv->print(s);
}

void drawPreview() {
  cv->fillRect(0, PREV_Y, SCREEN_W, PREV_H, 0x0000);
  if (!camOk) { textAt(&FreeSans9pt7b, 80, PREV_Y + 125, "Camera not found", rgb(255, 120, 110)); return; }
  if (!videoOn()) { textAt(&FreeSans9pt7b, 96, PREV_Y + 125, "Video is OFF", MUTED); return; }
  uint32_t len = previewLen;
  if (!previewWanted && len) {
    const Size &s = SIZES[sizeIdx];
    int div = s.scale == JPEG_SCALE_HALF ? 2 : 4;
    previewX = (SCREEN_W - s.w / div) / 2;
    previewY = PREV_Y + (PREV_H - s.h / div) / 2;
    if (jpeg.openRAM(previewBuf, len, drawJpegBlock)) {
      jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
      jpeg.decode(0, 0, s.scale);
      jpeg.close();
    }
  }
}

// Live acceleration graph (x red, y green, z blue).
void drawMotion() {
  cv->fillRoundRect(8, GRAPH_Y, 304, GRAPH_H, 8, PANEL);
  char buf[32];
  if (!imuOk) { small(18, GRAPH_Y + 22, "Motion sensor not found", rgb(255, 120, 110)); return; }
  Sample s;
  portENTER_CRITICAL(&latestLock);
  s = latest;
  portEXIT_CRITICAL(&latestLock);
  float now[3] = {s.ax, s.ay, s.az};
  small(14, GRAPH_Y + 5, "motion (g)", MUTED);
  for (int i = 0; i < 3; i++) {
    snprintf(buf, sizeof(buf), "%c %+.2f", "xyz"[i], now[i]);
    small(110 + i * 66, GRAPH_Y + 5, buf, AXIS_COL[i]);
  }
  int gx = 14, gw = 292, gy = GRAPH_Y + 16, gh = GRAPH_H - 20, mid = gy + gh / 2;
  cv->drawFastHLine(gx, mid, gw, 0x39C7);
  for (int a = 0; a < 3 && histN > 1; a++) {
    int px = 0, py = 0;
    for (int i = 0; i < histN; i++) {
      float v = constrain(hist[a][i], -2.0f, 2.0f);
      int xx = gx + i * gw / (HIST - 1), yy = mid - (int)(v / 2.0f * (gh / 2 - 1));
      if (i) cv->drawLine(px, py, xx, yy, AXIS_COL[a]);
      px = xx; py = yy;
    }
  }
}

void drawScreen() {
  cv->fillScreen(BG);
  textAt(&FreeSansBold12pt7b, 12, 31, "Recorder", INK);
  char buf[96];
  if (sdOk) snprintf(buf, sizeof(buf), "SD: %.1f GB free", sdFreeMB / 1024.0);
  else snprintf(buf, sizeof(buf), "no SD card");
  small(320 - 8 - strlen(buf) * 6, 20, buf, sdOk ? MUTED : rgb(255, 120, 110));

  drawPreview();
  if (recording) {
    bool blink = (millis() / 500) % 2;
    cv->drawRect(0, PREV_Y, SCREEN_W, PREV_H, rgb(230, 50, 50));
    cv->drawRect(1, PREV_Y + 1, SCREEN_W - 2, PREV_H - 2, rgb(230, 50, 50));
    cv->fillRoundRect(8, PREV_Y + 8, 58, 22, 6, rgb(0, 0, 0));
    cv->fillCircle(20, PREV_Y + 19, 5, blink ? rgb(240, 50, 50) : rgb(100, 30, 30));
    small(30, PREV_Y + 15, "REC", INK);
  }
  drawMotion();

  // Status
  cv->fillRoundRect(8, STAT_Y, 304, 36, 8, PANEL);
  if (recording) {
    float secs = (millis() - recStartMs) / 1000.0f;
    snprintf(buf, sizeof(buf), "%d:%02d", (int)secs / 60, (int)secs % 60);
    textAt(&FreeSansBold9pt7b, 16, STAT_Y + 22, buf, INK);
    if (videoOn())
      snprintf(buf, sizeof(buf), "%.1f fps  %.1f MB  lost %lu", secs > 0.5f ? framesSaved / secs : 0.0f,
               bytesSaved / 1048576.0f, (unsigned long)framesDropped);
    else
      snprintf(buf, sizeof(buf), "video off");
    small(76, STAT_Y + 7, buf, framesDropped ? rgb(255, 170, 120) : rgb(140, 230, 160));
    if (imuOn())
      snprintf(buf, sizeof(buf), "%lu motion rows  lost %lu", (unsigned long)(samplesSaved + (head - tail)),
               (unsigned long)samplesDropped);
    else
      snprintf(buf, sizeof(buf), "motion off");
    small(76, STAT_Y + 21, buf, samplesDropped ? rgb(255, 170, 120) : rgb(140, 230, 160));
  } else if (millis() < messageUntil) {
    small(16, STAT_Y + 14, message, rgb(255, 210, 120));
  } else if (lastSaved[0]) {
    small(16, STAT_Y + 7, "Last recording:", MUTED);
    small(16, STAT_Y + 21, lastSaved, MUTED);
  } else {
    small(16, STAT_Y + 14, "Pick video and motion settings, then tap REC.", MUTED);
  }

  // Options: tap to change
  const char *vid = videoOn() ? SIZES[sizeIdx].name : "OFF";
  snprintf(buf, sizeof(buf), "%d Hz", imuOn() ? RATES[rateIdx] : 0);
  const char *mot = imuOn() ? buf : "OFF";
  const char *labels[2] = {"Video", "Motion"};
  const char *values[2] = {vid, mot};
  for (int i = 0; i < 2; i++) {
    int x = 8 + i * 155;
    cv->fillRoundRect(x, OPT_Y, 149, 34, 8, recording ? BG : PANEL);
    small(x + 10, OPT_Y + 13, labels[i], MUTED);
    textAt(&FreeSansBold9pt7b, x + 58, OPT_Y + 23, values[i], recording ? 0x8410 : INK);
  }

  // REC / STOP
  cv->fillRoundRect(8, REC_Y, 304, 48, 12, recording ? rgb(80, 80, 96) : rgb(220, 50, 60));
  if (recording) cv->fillRect(122, REC_Y + 15, 18, 18, INK);
  else cv->fillCircle(130, REC_Y + 24, 10, INK);
  textAt(&FreeSansBold12pt7b, 150, REC_Y + 33, recording ? "STOP" : "REC", INK);
}

// ============================================================ touch

bool wasDown = false;

void nextVideoSize() {
  int next = (sizeIdx + 1) % (NUM_SIZES + 1);   // ... 1280x720, OFF, 640x480 ...
  if (next < NUM_SIZES) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s || s->set_framesize(s, SIZES[next].fs) != 0) return;
  }
  sizeIdx = next;
  prefs.putInt("size", sizeIdx);
  previewWanted = true;
}

void nextMotionRate() {
  rateIdx = (rateIdx + 1) % (NUM_RATES + 1);    // ... 200 Hz, OFF, 25 Hz ...
  imuHz = imuOn() ? RATES[rateIdx] : 50;        // still read at 50 Hz for the live graph
  prefs.putInt("rate", rateIdx);
}

void handleTouch() {
  if (!touchOk) return;
  int16_t xs[1], ys[1];
  bool down = touch.getPoint(xs, ys, 1) > 0;
  if (down && !wasDown) {
    int x = xs[0], y = ys[0];
    if (y >= REC_Y && y <= REC_Y + 48) {
      if (recording) stopRecording();
      else startRecording();
    } else if (y >= OPT_Y && y <= OPT_Y + 34 && !recording) {
      if (x < 160) nextVideoSize();
      else nextMotionRate();
    }
  }
  wasDown = down;
}

// ============================================================ setup & loop

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  BoardStatus st = boardPowerUp(expander, pmu);   // also powers the camera (power chip BLDO1/BLDO2)

  bool lcdOk = lcd->begin(LCD_SPI_HZ, SPI_MODE0);
  screenOk = lcdOk && cv->begin(GFX_SKIP_OUTPUT_BEGIN);
  touchOk = touch.begin(Wire, FT6X36_SLAVE_ADDRESS);

  imuOk = imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS);
  if (imuOk) {
    // The sensor runs faster than we read it, with its low-pass filter on.
    imu.configAccelerometer(SensorQMI8658::ACC_RANGE_8G, SensorQMI8658::ACC_ODR_500Hz, SensorQMI8658::LPF_MODE_3);
    imu.configGyroscope(SensorQMI8658::GYR_RANGE_1024DPS, SensorQMI8658::GYR_ODR_448_4Hz, SensorQMI8658::LPF_MODE_3);
    imu.enableAccelerometer();
    imu.enableGyroscope();
  }

  prefs.begin("recorder", false);
  fileCounter = prefs.getUInt("n", 1);
  sizeIdx = constrain(prefs.getInt("size", 0), 0, NUM_SIZES);
  rateIdx = constrain(prefs.getInt("rate", 2), 0, NUM_RATES);
  imuHz = imuOn() ? RATES[rateIdx] : 50;

  slotMem = (uint8_t *)ps_malloc((size_t)SLOTS * SLOT_BYTES);
  previewBuf = (uint8_t *)ps_malloc(SLOT_BYTES);
  ring = (Sample *)ps_malloc(RING * sizeof(Sample));
  freeSlots = xQueueCreate(SLOTS, sizeof(int));
  fullSlots = xQueueCreate(SLOTS, sizeof(int));
  for (int i = 0; slotMem && i < SLOTS; i++) xQueueSend(freeSlots, &i, 0);
  if (!ring) imuOk = false;

  camOk = slotMem && previewBuf && startCamera();
  mountCard();

  Serial.printf("[boot] Recorder build %s | PSRAM %s (%u KB free) | camera %s | motion sensor %s | touch %s | SD %s\n",
                FIRMWARE_BUILD, st.psram ? "found" : "NOT FOUND", (unsigned)(ESP.getFreePsram() / 1024),
                camOk ? "ok" : "FAILED", imuOk ? "ok" : "MISSING", touchOk ? "ok" : "MISSING", sdOk ? "ok" : "none");

  if (imuOk) xTaskCreatePinnedToCore(imuTask, "imu", 4096, nullptr, configMAX_PRIORITIES - 1, nullptr, 0);
  xTaskCreatePinnedToCore(captureTask, "capture", 4096, nullptr, configMAX_PRIORITIES - 2, nullptr, 0);
  xTaskCreatePinnedToCore(writerTask, "writer", 8192, nullptr, configMAX_PRIORITIES - 3, nullptr, 1);
}

uint32_t lastDraw = 0;

void loop() {
  handleTouch();
  if (recording && (writeFailed || (millis() - recStartMs) / 1000 >= MAX_SECONDS)) stopRecording();

  if (screenOk && millis() - lastDraw >= 100) {   // about 10 screen updates a second
    lastDraw = millis();
    Sample s;
    portENTER_CRITICAL(&latestLock);
    s = latest;
    portEXIT_CRITICAL(&latestLock);
    if (histN == HIST) {
      for (int a = 0; a < 3; a++) memmove(hist[a], hist[a] + 1, sizeof(float) * (HIST - 1));
      histN--;
    }
    hist[0][histN] = s.ax; hist[1][histN] = s.ay; hist[2][histN] = s.az;
    histN++;
    drawScreen();
    cv->flush();
    previewWanted = true;
    static bool backlightOn = false;
    if (!backlightOn) { boardBacklight(true); backlightOn = true; }
  }
  delay(5);
}
