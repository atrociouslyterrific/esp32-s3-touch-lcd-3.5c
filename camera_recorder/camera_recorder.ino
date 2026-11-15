// CAMERA RECORDER: record video from the board's camera to a microSD card at 20 fps.
// For the Waveshare ESP32-S3-Touch-LCD-3.5-C (OV5640 camera).
//
// - The screen shows what the camera sees. Pick a size and tap REC; tap STOP to finish.
// - Videos are saved in the card's /video folder as vid_0001.avi and so on:
//   MJPEG .avi files that play in VLC, QuickTime, Windows Media Player and most editors.
// - It aims for exactly 20 frames a second. The screen shows the rate it really
//   managed; big sizes may come out lower if the card can't keep up.
// - The longest recording is 60 minutes (or until the card is full).
//
// How it keeps a steady 20 fps: one task takes a picture every 50 ms and copies it
// into a waiting slot in the board's PSRAM; a second task writes the waiting
// pictures to the card. So a slow moment on the card doesn't make it skip frames
// unless all the slots fill up.

#include <Arduino_GFX_Library.h>
#include "board.h"
#include <TouchDrvFT6X36.hpp>
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
Preferences prefs;
JPEGDEC jpeg;
Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, -1, SPI_SCLK, SPI_MOSI, SPI_MISO);
Arduino_ST7796 *lcd = new Arduino_ST7796(bus, -1, 0, true, SCREEN_W, SCREEN_H);
Arduino_Canvas *cv = new Arduino_Canvas(SCREEN_W, SCREEN_H, lcd, 0, 0, 0);
bool camOk = false, touchOk = false, screenOk = false, sdOk = false;

// ---- Sizes ----
struct Size { framesize_t fs; uint16_t w, h; const char *name; int scale; };
const Size SIZES[3] = {
  {FRAMESIZE_VGA, 640, 480, "640x480", JPEG_SCALE_HALF},
  {FRAMESIZE_SVGA, 800, 600, "800x600", JPEG_SCALE_QUARTER},
  {FRAMESIZE_HD, 1280, 720, "1280x720", JPEG_SCALE_QUARTER},
};
int sizeIdx = 0;

// ---- Frame slots in PSRAM ----
const int SLOTS = 24;                     // about 1.2 s of video waiting room (4.6 MB)
const uint32_t SLOT_BYTES = 192 * 1024;   // biggest frame we keep (bigger ones are dropped)
uint8_t *slotMem = nullptr;
uint32_t slotLen[SLOTS];
QueueHandle_t freeSlots, fullSlots;

// ---- Preview ----
uint8_t *previewBuf = nullptr;            // copy of a recent frame for the screen
volatile uint32_t previewLen = 0;
volatile bool previewWanted = true;

// ---- Recording state (shared between tasks) ----
volatile bool recording = false;
volatile bool stopAsked = false;
volatile uint32_t framesTaken = 0, framesSaved = 0, framesDropped = 0, bytesSaved = 0;
volatile bool writeFailed = false;
AviWriter avi;
char fileName[32] = "";
char lastSaved[64] = "";
uint32_t recStartUs = 0, recStartMs = 0;
uint32_t fileCounter = 1;
uint64_t sdFreeMB = 0;
const char *message = "";
uint32_t messageUntil = 0;

static inline uint16_t rgb(int r, int g, int b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
void say(const char *m) { message = m; messageUntil = millis() + 4000; Serial.println(m); }

// ============================================================ camera

bool startCamera(int idx) {
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
  // Start at the biggest size so the camera's buffers fit any size, then switch down.
  c.frame_size = FRAMESIZE_HD;
  c.pixel_format = PIXFORMAT_JPEG;
  c.grab_mode = CAMERA_GRAB_LATEST;   // always the newest picture
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.jpeg_quality = 12;
  c.fb_count = 2;
  if (esp_camera_init(&c) != ESP_OK) return false;
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, 1);              // the camera is mounted upside down (as in Waveshare's firmware)
    s->set_framesize(s, SIZES[idx].fs);
  }
  return true;
}

// Takes a picture every 50 ms. Runs on its own core at high priority.
void captureTask(void *) {
  TickType_t last = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(1000 / FPS);
  while (true) {
    if (camOk) {
      camera_fb_t *fb = esp_camera_fb_get();
      if (fb) {
        if (recording && !stopAsked) {
          framesTaken = framesTaken + 1;
          int slot;
          if (fb->len + 10 <= SLOT_BYTES && xQueueReceive(freeSlots, &slot, 0) == pdTRUE) {
            memcpy(slotMem + (size_t)slot * SLOT_BYTES + 8, fb->buf, fb->len);   // 8 bytes left for the chunk header
            slotLen[slot] = fb->len;
            xQueueSend(fullSlots, &slot, 0);
          } else {
            framesDropped = framesDropped + 1;
          }
        }
        if (previewWanted && fb->len <= SLOT_BYTES) {   // a copy for the screen
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

// Writes waiting pictures to the card.
void writerTask(void *) {
  uint32_t lastFlush = millis();
  while (true) {
    int slot;
    if (xQueueReceive(fullSlots, &slot, pdMS_TO_TICKS(20)) == pdTRUE) {
      if (!writeFailed) {
        if (avi.addFrame(slotMem + (size_t)slot * SLOT_BYTES, slotLen[slot])) {
          framesSaved = framesSaved + 1;
          bytesSaved = bytesSaved + slotLen[slot] + 8;
        } else {
          writeFailed = true;   // card full or removed
        }
      }
      xQueueSend(freeSlots, &slot, 0);
      if (millis() - lastFlush > 2000 && avi.f) { avi.f.flush(); lastFlush = millis(); }
    }
  }
}

// ============================================================ card and recording

bool mountCard() {
  if (sdOk) return true;
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_HIGHSPEED)) return false;
  if (SD_MMC.cardType() == CARD_NONE) { SD_MMC.end(); return false; }
  SD_MMC.mkdir("/video");
  sdOk = true;
  sdFreeMB = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024);
  return true;
}

void startRecording() {
  if (!camOk) { say("Camera not found."); return; }
  if (!mountCard()) { say("No microSD card. Put one in and tap REC."); return; }
  for (int tries = 0; tries < 10000; tries++) {
    snprintf(fileName, sizeof(fileName), "/video/vid_%04lu.avi", (unsigned long)fileCounter);
    if (!SD_MMC.exists(fileName)) break;
    fileCounter++;
  }
  File f = SD_MMC.open(fileName, FILE_WRITE);
  if (!f) { sdOk = false; say("Couldn't create a file on the card."); return; }
  if (!avi.begin(f, SIZES[sizeIdx].w, SIZES[sizeIdx].h, FPS * MAX_SECONDS + FPS * 10)) {
    f.close();
    say("Not enough memory to record.");
    return;
  }
  fileCounter++;
  prefs.putUInt("n", fileCounter);
  framesTaken = framesSaved = framesDropped = bytesSaved = 0;
  writeFailed = false;
  stopAsked = false;
  recStartUs = (uint32_t)esp_timer_get_time();
  recStartMs = millis();
  recording = true;
  Serial.printf("Recording %s at %s, %d fps\n", fileName, SIZES[sizeIdx].name, FPS);
}

void stopRecording() {
  stopAsked = true;                          // no new frames
  uint32_t elapsedUs = (uint32_t)esp_timer_get_time() - recStartUs;
  uint32_t t0 = millis();
  while (uxQueueMessagesWaiting(fullSlots) > 0 && millis() - t0 < 10000) delay(10);   // write what's left
  delay(60);
  recording = false;
  // The file's frame rate is the real one, so it plays back at the right speed.
  uint32_t effectiveUs = framesSaved ? elapsedUs : 0;
  avi.end(effectiveUs);
  float secs = elapsedUs / 1e6f;
  snprintf(lastSaved, sizeof(lastSaved), "%s: %lu frames, %.1f fps", fileName + 7, (unsigned long)framesSaved,
           secs > 0 ? framesSaved / secs : 0);
  sdFreeMB = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024);
  Serial.printf("Saved %s: %lu frames in %.1f s (%lu dropped)\n", fileName, (unsigned long)framesSaved, secs,
                (unsigned long)framesDropped);
  say(writeFailed ? "Stopped: the card is full or was removed." : "Saved!");
}

// ============================================================ drawing

const uint16_t BG = 0x10A3, PANEL = 0x18E5, INK = 0xFFFF, MUTED = 0xAD55;
const int PREV_Y = 48, PREV_H = 240, STAT_Y = 296, SIZE_Y = 384, REC_Y = 426;
int previewX = 0, previewY = 0;

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
  uint32_t len = previewLen;
  if (!previewWanted && len) {
    const Size &s = SIZES[sizeIdx];
    int div = s.scale == JPEG_SCALE_HALF ? 2 : 4;
    int w = s.w / div, h = s.h / div;
    previewX = (SCREEN_W - w) / 2;
    previewY = PREV_Y + (PREV_H - h) / 2;
    if (jpeg.openRAM(previewBuf, len, drawJpegBlock)) {
      jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
      jpeg.decode(0, 0, s.scale);
      jpeg.close();
    }
  }
}

void drawScreen() {
  cv->fillScreen(BG);
  textAt(&FreeSansBold12pt7b, 12, 32, "Camera", INK);
  char buf[72];
  if (sdOk) snprintf(buf, sizeof(buf), "SD: %.1f GB free", sdFreeMB / 1024.0);
  else snprintf(buf, sizeof(buf), "no SD card");
  small(320 - 8 - strlen(buf) * 6, 22, buf, sdOk ? MUTED : rgb(255, 120, 110));

  drawPreview();
  if (recording) {   // red frame + REC tag over the preview
    bool blink = (millis() / 500) % 2;
    cv->drawRect(0, PREV_Y, SCREEN_W, PREV_H, rgb(230, 50, 50));
    cv->drawRect(1, PREV_Y + 1, SCREEN_W - 2, PREV_H - 2, rgb(230, 50, 50));
    cv->fillRoundRect(8, PREV_Y + 8, 58, 22, 6, rgb(0, 0, 0));
    cv->fillCircle(20, PREV_Y + 19, 5, blink ? rgb(240, 50, 50) : rgb(100, 30, 30));
    small(30, PREV_Y + 15, "REC", INK);
  }

  // Stats
  cv->fillRoundRect(8, STAT_Y, 304, 80, 10, PANEL);
  if (recording) {
    float secs = (millis() - recStartMs) / 1000.0f;
    snprintf(buf, sizeof(buf), "%d:%02d", (int)secs / 60, (int)secs % 60);
    textAt(&FreeSansBold12pt7b, 20, STAT_Y + 30, buf, INK);
    snprintf(buf, sizeof(buf), "%.1f fps", secs > 0.5f ? framesSaved / secs : 0.0f);
    textAt(&FreeSansBold9pt7b, 200, STAT_Y + 28, buf, framesDropped ? rgb(255, 170, 120) : rgb(140, 230, 160));
    snprintf(buf, sizeof(buf), "%s  |  %.1f MB  |  %lu frames  |  lost %lu", fileName + 7, bytesSaved / 1048576.0f,
             (unsigned long)framesSaved, (unsigned long)framesDropped);
    small(20, STAT_Y + 46, buf, MUTED);
    snprintf(buf, sizeof(buf), "%u of %d slots waiting for the card", (unsigned)uxQueueMessagesWaiting(fullSlots), SLOTS);
    small(20, STAT_Y + 60, buf, MUTED);
  } else {
    textAt(&FreeSansBold12pt7b, 20, STAT_Y + 30, "Ready", INK);
    if (millis() < messageUntil) small(20, STAT_Y + 46, message, rgb(255, 210, 120));
    else small(20, STAT_Y + 46, "Pick a size, then tap REC. 20 fps.", MUTED);
    if (lastSaved[0]) { snprintf(buf, sizeof(buf), "Last: %s", lastSaved); small(20, STAT_Y + 60, buf, MUTED); }
  }

  // Size buttons
  for (int i = 0; i < 3; i++) {
    int x = 8 + i * 103;
    bool on = i == sizeIdx;
    cv->fillRoundRect(x, SIZE_Y, 98, 34, 8, on ? rgb(70, 130, 220) : (recording ? BG : PANEL));
    int w = textWidth(&FreeSans9pt7b, SIZES[i].name);
    textAt(&FreeSans9pt7b, x + 49 - w / 2, SIZE_Y + 23, SIZES[i].name, recording && !on ? 0x528A : INK);
  }

  // REC / STOP
  cv->fillRoundRect(8, REC_Y, 304, 48, 12, recording ? rgb(80, 80, 96) : rgb(220, 50, 60));
  if (recording) cv->fillRect(122, REC_Y + 15, 18, 18, INK);
  else cv->fillCircle(130, REC_Y + 24, 10, INK);
  textAt(&FreeSansBold12pt7b, 150, REC_Y + 33, recording ? "STOP" : "REC", INK);
}

// ============================================================ touch

bool wasDown = false;

void changeSize(int i) {
  if (i == sizeIdx || recording) return;
  sensor_t *s = esp_camera_sensor_get();
  if (s && s->set_framesize(s, SIZES[i].fs) == 0) {
    sizeIdx = i;
    prefs.putInt("size", i);
    previewWanted = true;
  }
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
    } else if (y >= SIZE_Y && y <= SIZE_Y + 34) {
      changeSize((x - 8) / 103);
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

  prefs.begin("camrec", false);
  fileCounter = prefs.getUInt("n", 1);
  sizeIdx = constrain(prefs.getInt("size", 0), 0, 2);

  slotMem = (uint8_t *)ps_malloc((size_t)SLOTS * SLOT_BYTES);
  previewBuf = (uint8_t *)ps_malloc(SLOT_BYTES);
  freeSlots = xQueueCreate(SLOTS, sizeof(int));
  fullSlots = xQueueCreate(SLOTS, sizeof(int));
  for (int i = 0; slotMem && i < SLOTS; i++) xQueueSend(freeSlots, &i, 0);

  camOk = slotMem && previewBuf && startCamera(sizeIdx);
  mountCard();

  Serial.printf("[boot] Camera recorder build %s | PSRAM %s (%u KB free) | camera %s | touch %s | SD %s\n",
                FIRMWARE_BUILD, st.psram ? "found" : "NOT FOUND", (unsigned)(ESP.getFreePsram() / 1024),
                camOk ? "ok" : "FAILED", touchOk ? "ok" : "MISSING", sdOk ? "ok" : "none");

  xTaskCreatePinnedToCore(captureTask, "capture", 4096, nullptr, configMAX_PRIORITIES - 2, nullptr, 0);
  xTaskCreatePinnedToCore(writerTask, "writer", 6144, nullptr, configMAX_PRIORITIES - 3, nullptr, 1);
}

uint32_t lastDraw = 0;

void loop() {
  handleTouch();
  if (recording && (writeFailed || (millis() - recStartMs) / 1000 >= MAX_SECONDS)) stopRecording();

  if (screenOk && millis() - lastDraw >= 100) {   // about 10 screen updates a second
    lastDraw = millis();
    drawScreen();
    cv->flush();
    previewWanted = true;                          // ask for a fresh picture for next time
    static bool backlightOn = false;
    if (!backlightOn) { boardBacklight(true); backlightOn = true; }
  }
  delay(5);
}
