// RECORDER: video from the camera and data from the motion sensor (IMU), recorded
// together onto a microSD card. For the Waveshare ESP32-S3-Touch-LCD-3.5-C.
//
// - The screen shows the camera and a live motion graph. Pick a video size and a
//   motion rate (either can be OFF), then tap REC. Tap STOP to finish.
//   FLIP turns the picture if it's upside down or mirrored (remembered).
// - Each recording makes these files in the card's /rec folder:
//     rec_2026-11-14_19-05-12.avi         video at 20 frames a second (MJPEG; VLC, QuickTime...)
//     rec_2026-11-14_19-05-12_imu.csv     time_s, unix_ms, ax_g, ay_g, az_g, gx_dps, gy_dps, gz_dps
//     rec_2026-11-14_19-05-12_frames.csv  frame, time_s, unix_ms   (when each video frame was taken)
//     rec_2026-11-14_19-05-12_info.txt    when it started, settings, what was saved
//   (Named rec_0001... instead if the board's clock hasn't been set yet.)
//   time_s = seconds since REC, on the same clock for video and motion, so they line up.
//   unix_ms = the real date and time (milliseconds since 1 Jan 1970, UTC), from the
//   board's clock chip. Connect the board to the website once to set its clock.
// - Motion: acceleration in g (1 g = gravity), up to +/-8 g; rotation in degrees per
//   second, up to +/-1024. Axes are the sensor chip's own axes.
// - Files are saved every couple of seconds. Longest recording: 60 minutes.
// - The card must be formatted FAT32 (cards over 32 GB often come as exFAT).
//
// Separate tasks read the motion sensor and take a picture every 50 ms on fixed beats,
// into waiting rooms in PSRAM; a writer task saves everything to the card. The camera
// preview is drawn straight to the screen, so it can keep up with the camera.

#include <Arduino_GFX_Library.h>
#include "board.h"
#include <TouchDrvFT6X36.hpp>
#include <SensorQMI8658.hpp>
#include <SensorPCF85063.hpp>
#include <esp_camera.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <JPEGDEC.h>
#include <WiFi.h>
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

// ---- Screen layout: header and controls are drawn off-screen and sent now and
// then; the camera preview in between goes straight to the screen. ----
const int HEAD_H = 44;
const int PREV_Y = 46, PREV_H = 240;
const int LOW_Y = 288, LOW_H = SCREEN_H - LOW_Y;   // controls area
// (positions below are inside the controls area)
const int GRAPH_Y = 2, GRAPH_H = 46, STAT_Y = 52, OPT_Y = 92, REC_Y = 134;

// ---- Hardware ----
TCA9554 expander(0x20);
XPowersPMU pmu;
TouchDrvFT6X36 touch;
SensorQMI8658 imu;
SensorPCF85063 rtc;
Preferences prefs;
JPEGDEC jpeg;
Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, -1, SPI_SCLK, SPI_MOSI, SPI_MISO);
Arduino_ST7796 *lcd = new Arduino_ST7796(bus, -1, 0, true, SCREEN_W, SCREEN_H);
Arduino_Canvas *head = new Arduino_Canvas(SCREEN_W, HEAD_H, lcd, 0, 0, 0);
Arduino_Canvas *low = new Arduino_Canvas(SCREEN_W, LOW_H, lcd, 0, LOW_Y, 0);
bool camOk = false, imuOk = false, touchOk = false, screenOk = false, sdOk = false, rtcOk = false;

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
int orient = 0;           // 0..3: which way the picture is turned (FLIP)
bool videoOn() { return sizeIdx < NUM_SIZES; }
bool imuOn() { return rateIdx < NUM_RATES; }

// ---- Video waiting room: frame slots in PSRAM ----
const int SLOTS = 24;                     // about 1.2 s of video (4.6 MB)
const uint32_t SLOT_BYTES = 192 * 1024;
uint8_t *slotMem = nullptr;
uint32_t slotLen[SLOTS], slotTimeUs[SLOTS];
QueueHandle_t freeSlots, fullSlots;

// ---- Motion waiting room ----
struct Sample { uint32_t tUs; float ax, ay, az, gx, gy, gz; };
const uint32_t RING = 16384;              // about 80 s at 200 Hz
Sample *ring = nullptr;
volatile uint32_t head_ = 0, tail_ = 0;
Sample latest = {};
portMUX_TYPE latestLock = portMUX_INITIALIZER_UNLOCKED;
volatile int imuHz = 100;

// ---- Preview ----
uint8_t *previewBuf = nullptr;
volatile uint32_t previewLen = 0;
volatile bool previewWanted = true;
uint32_t previewFrames = 0, previewFpsAt = 0;
volatile uint32_t cameraFrames = 0;   // new pictures from the camera (for its own fps)
uint32_t cameraFramesAt = 0;
float cameraFps = 0;
float previewFps = 0;

// ---- Real date and time ----
int64_t unixBaseMs = 0;      // unix time (ms) = unixBaseMs + esp_timer ms, once the clock is known
bool clockKnown = false;
int tzMinutes = 0;           // your time zone, from the website (for file names)

// ---- Recording state (shared between tasks) ----
volatile bool recording = false;
volatile bool stopAsked = false;
volatile bool writerIdle = true;
volatile uint32_t framesSaved = 0, framesDropped = 0, bytesSaved = 0, samplesSaved = 0, samplesDropped = 0;
volatile bool writeFailed = false;
AviWriter avi;
File imuFile, frameFile;
char baseName[48] = "";
char lastSaved[80] = "";
uint32_t recStartUs = 0, recStartMs = 0;
int64_t recStartUnixMs = 0;
uint32_t fileCounter = 1;
uint64_t sdFreeMB = 0;
const char *sdProblem = "";
char message[80] = "";
uint32_t messageUntil = 0;

static inline uint16_t rgb(int r, int g, int b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
void say(const char *m) { snprintf(message, sizeof(message), "%s", m); messageUntil = millis() + 5000; Serial.println(m); }

// ============================================================ date and time

int64_t nowUnixMs() { return unixBaseMs + esp_timer_get_time() / 1000; }

// Days since 1970-01-01 for a calendar date (and back), without time zones.
int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}
void civilFromDays(int64_t z, int &y, int &m, int &d) {
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int doe = z - era * 146097;
  int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : -9);
  y = yoe + era * 400 + (m <= 2);
}
// "2026-11-14_19-05-12" in your local time (sep '_' and '-'), or with ' ' and ':' for people.
void formatLocal(int64_t unixMs, char *out, size_t n, bool forFileName) {
  int64_t s = unixMs / 1000 + tzMinutes * 60;
  int64_t days = s / 86400;
  int rem = s % 86400;
  int y, mo, d;
  civilFromDays(days, y, mo, d);
  if (forFileName) snprintf(out, n, "%04d-%02d-%02d_%02d-%02d-%02d", y, mo, d, rem / 3600, rem / 60 % 60, rem % 60);
  else snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, rem / 3600, rem / 60 % 60, rem % 60);
}

void readClockChip() {
  if (!rtcOk) return;
  RTC_DateTime t = rtc.getDateTime();
  if (t.getYear() < 2025 || t.getYear() > 2099) return;   // never set
  int64_t unixS = daysFromCivil(t.getYear(), t.getMonth(), t.getDay()) * 86400 + t.getHour() * 3600 +
                  t.getMinute() * 60 + t.getSecond();
  unixBaseMs = unixS * 1000 - esp_timer_get_time() / 1000;
  clockKnown = true;
}

// From the website: "settime <unix ms> <minutes east of UTC>"
void setClock(int64_t unixMs, int tz) {
  unixBaseMs = unixMs - esp_timer_get_time() / 1000;
  clockKnown = true;
  tzMinutes = tz;
  prefs.putInt("tz", tz);
  if (rtcOk) {   // the clock chip keeps UTC
    int64_t s = unixMs / 1000;
    int y, mo, d;
    civilFromDays(s / 86400, y, mo, d);
    int rem = s % 86400;
    rtc.setDateTime(y, mo, d, rem / 3600, rem / 60 % 60, rem % 60);
  }
  char buf[32];
  formatLocal(unixMs, buf, sizeof(buf), false);
  Serial.printf("{\"t\":\"clock\",\"set\":\"%s\"}\n", buf);
}

// ============================================================ camera

void applyOrientation() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;
  // 0: turned 180 degrees from the camera's own way up (right for this board's screen),
  // 1: camera's own way, 2 and 3: mirrored versions.
  const uint8_t vflip[4] = {0, 1, 1, 0}, hmirror[4] = {1, 0, 1, 0};
  s->set_vflip(s, vflip[orient]);
  s->set_hmirror(s, hmirror[orient]);
}

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
  c.fb_count = 3;               // a spare buffer keeps the camera running while we copy
  if (esp_camera_init(&c) != ESP_OK) return false;
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, SIZES[videoOn() ? sizeIdx : 0].fs);
    // Free picture improvements: even brightness to the corners (lens correction),
    // hide stuck pixels, automatic exposure, gain and white balance.
    if (s->set_lenc) s->set_lenc(s, 1);
    if (s->set_bpc) s->set_bpc(s, 1);
    if (s->set_wpc) s->set_wpc(s, 1);
    if (s->set_exposure_ctrl) s->set_exposure_ctrl(s, 1);
    if (s->set_gain_ctrl) s->set_gain_ctrl(s, 1);
    if (s->set_whitebal) s->set_whitebal(s, 1);
    if (s->set_awb_gain) s->set_awb_gain(s, 1);
    // Less grain: cap how much the sensor amplifies in dim light (a little darker,
    // much less noisy) and switch on its noise reduction.
    if (s->set_gainceiling) s->set_gainceiling(s, GAINCEILING_8X);
    if (s->set_denoise) s->set_denoise(s, 2);
    // Less banding under indoor lights: switch on the OV5640's light-flicker filter
    // (register 0x3A00 bit 5), which times the exposure to the mains flicker.
    if (s->set_reg) s->set_reg(s, 0x3A00, 0x20, 0x20);
  }
  applyOrientation();
  resetQuality();
  return true;
}

// ---- Picture quality ----
// JPEG quality: smaller number = sharper picture, bigger file. Each size starts at
// the best quality a card can usually keep up with at 20 fps. While recording, if
// pictures pile up waiting for the card, quality steps down a little so it keeps
// 20 fps instead of losing frames; when the card catches up, it steps back up.
const int BEST_QUALITY[NUM_SIZES] = {8, 10, 12};
const int WORST_QUALITY = 30;
int quality = 12;
uint32_t qualityCheckMs = 0, calmSinceMs = 0, droppedAtCheck = 0;

void setQuality(int q) {
  quality = q;
  sensor_t *s = esp_camera_sensor_get();
  if (s) s->set_quality(s, q);
}

void resetQuality() {
  setQuality(BEST_QUALITY[videoOn() ? sizeIdx : 0]);
  calmSinceMs = millis();
}

void autoQuality() {
  if (!recording || !videoOn() || millis() - qualityCheckMs < 500) return;
  qualityCheckMs = millis();
  int waiting = uxQueueMessagesWaiting(fullSlots);
  bool lost = framesDropped != droppedAtCheck;
  droppedAtCheck = framesDropped;
  if (waiting > SLOTS / 3 || lost) {          // the card is falling behind
    if (quality < WORST_QUALITY) setQuality(min(quality + 2, WORST_QUALITY));
    calmSinceMs = millis();
  } else if (waiting <= 1) {
    if (millis() - calmSinceMs > 3000 && quality > BEST_QUALITY[sizeIdx]) {   // calm for 3 s: step back up
      setQuality(quality - 1);
      calmSinceMs = millis();
    }
  } else {
    calmSinceMs = millis();
  }
}

// A complete JPEG starts with FF D8 and ends with FF D9 (sometimes followed by a few
// padding bytes). A picture damaged on its way from the camera (that's what shows up
// as lines or stripes) fails this check.
volatile uint32_t damagedFrames = 0;
bool jpegLooksWhole(const uint8_t *b, size_t len) {
  if (len < 128 || b[0] != 0xFF || b[1] != 0xD8) return false;
  for (size_t i = len - 1; i > len - 64 && i > 1; i--)
    if (b[i] == 0xD9 && b[i - 1] == 0xFF) return true;
  return false;
}

// Takes a picture every 50 ms.
void captureTask(void *) {
  TickType_t last = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(1000 / FPS);
  while (true) {
    if (camOk) {
      camera_fb_t *fb = esp_camera_fb_get();
      if (fb && !jpegLooksWhole(fb->buf, fb->len)) {   // damaged: drop it and take the next one
        damagedFrames = damagedFrames + 1;
        esp_camera_fb_return(fb);
        fb = esp_camera_fb_get();
        if (fb && !jpegLooksWhole(fb->buf, fb->len)) {
          damagedFrames = damagedFrames + 1;
          esp_camera_fb_return(fb);
          fb = nullptr;
        }
      }
      if (fb) {
        uint32_t t = (uint32_t)esp_timer_get_time();
        static int64_t lastStamp = 0;   // a new picture, or the same one again?
        int64_t stamp = (int64_t)fb->timestamp.tv_sec * 1000000 + fb->timestamp.tv_usec;
        if (stamp != lastStamp) { lastStamp = stamp; cameraFrames = cameraFrames + 1; }
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
      if (head_ - tail_ < RING) {
        ring[head_ % RING] = s;
        head_ = head_ + 1;
      } else {
        samplesDropped = samplesDropped + 1;
      }
    }
    vTaskDelayUntil(&last, period);
  }
}

// ============================================================ Wi-Fi scanner
// Lists the Wi-Fi networks nearby (it never joins one). Scans every 5 seconds while
// switched on. While recording, each scan is also saved to _wifi.csv.

struct Net { char ssid[33]; uint8_t bssid[6]; int8_t rssi; uint8_t ch; bool open; };
const int MAX_NETS = 24;
Net nets[MAX_NETS];
int netCount = 0;
bool wifiOn = false, wifiScanning = false, wifiListDirty = false;
uint32_t wifiLastScanMs = 0, wifiScans = 0;
int64_t wifiLastScanUnixMs = 0;
// Rows waiting for the writer task (it owns the card)
char wifiRows[6144];
volatile int wifiRowsLen = 0;
volatile bool wifiRowsReady = false;
File wifiFile;

// Network names can contain anything; keep them safe for CSV and the website.
void cleanName(const char *in, char *out, size_t n) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j < n - 1; i++) {
    unsigned char c = in[i];
    out[j++] = (c < 32 || c == '"' || c == '\\' || c == ',' || c > 126) ? ' ' : c;
  }
  out[j] = 0;
}

void wifiStartScan() {
  WiFi.scanNetworks(true, true);   // in the background; include hidden networks
  wifiScanning = true;
}

void wifiSet(bool on) {
  wifiOn = on;
  if (on) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    wifiStartScan();
  } else {
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
    wifiScanning = false;
    netCount = 0;
  }
  prefs.putBool("wifi", on);
  wifiListDirty = true;
}

void wifiPoll() {
  if (!wifiOn) return;
  if (!wifiScanning) {
    if (millis() - wifiLastScanMs > 5000) wifiStartScan();
    return;
  }
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  wifiScanning = false;
  wifiLastScanMs = millis();
  if (n < 0) return;   // failed; try again in 5 s
  netCount = 0;
  for (int i = 0; i < n && netCount < MAX_NETS; i++) {
    Net &w = nets[netCount++];
    cleanName(WiFi.SSID(i).c_str(), w.ssid, sizeof(w.ssid));
    if (!w.ssid[0]) strcpy(w.ssid, "(hidden)");
    memcpy(w.bssid, WiFi.BSSID(i), 6);
    w.rssi = WiFi.RSSI(i);
    w.ch = WiFi.channel(i);
    w.open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  // Strongest first
  for (int i = 1; i < netCount; i++)
    for (int j = i; j > 0 && nets[j].rssi > nets[j - 1].rssi; j--) { Net t = nets[j]; nets[j] = nets[j - 1]; nets[j - 1] = t; }
  wifiScans++;
  wifiLastScanUnixMs = clockKnown ? nowUnixMs() : 0;
  wifiListDirty = true;

  // To the website
  if (Serial) {
    Serial.print("{\"t\":\"wifi\",\"n\":[");
    for (int i = 0; i < netCount; i++)
      Serial.printf("%s[\"%s\",%d,%d,%d]", i ? "," : "", nets[i].ssid, nets[i].rssi, nets[i].ch, nets[i].open ? 1 : 0);
    Serial.println("]}");
  }
  // To the card, while recording
  if (recording && !stopAsked && !wifiRowsReady) {
    uint32_t t = (uint32_t)esp_timer_get_time();
    float secs = (uint32_t)(t - recStartUs) / 1e6f;
    long long u = clockKnown ? recStartUnixMs + (uint32_t)(t - recStartUs) / 1000 : 0;
    char when[32] = "";
    if (clockKnown) formatLocal(u, when, sizeof(when), false);
    int len = 0;
    for (int i = 0; i < netCount && len < (int)sizeof(wifiRows) - 160; i++) {
      const Net &w = nets[i];
      char bssid[18];
      snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X", w.bssid[0], w.bssid[1], w.bssid[2], w.bssid[3], w.bssid[4], w.bssid[5]);
      if (clockKnown)
        len += snprintf(wifiRows + len, sizeof(wifiRows) - len, "%.3f,%lld,%s,%s,%s,%d,%d,%s\n", secs, u, when, w.ssid, bssid, w.ch, w.rssi, w.open ? "open" : "locked");
      else
        len += snprintf(wifiRows + len, sizeof(wifiRows) - len, "%.3f,,,%s,%s,%d,%d,%s\n", secs, w.ssid, bssid, w.ch, w.rssi, w.open ? "open" : "locked");
    }
    wifiRowsLen = len;
    wifiRowsReady = len > 0;
  }
}

// ============================================================ writing to the card

// Seconds since REC (unsigned difference: fine for the whole 60 minutes).
static inline float secsSinceStart(uint32_t tUs) { return (uint32_t)(tUs - recStartUs) / 1e6f; }
static inline long long unixAt(uint32_t tUs) { return recStartUnixMs + (uint32_t)(tUs - recStartUs) / 1000; }

// Readable local time with milliseconds, e.g. "2026-11-14 19:05:12.345".
// The date part only changes once a second, so it's worked out once a second.
const char *localStamp(long long unixMs) {
  static long long cachedSec = -1;
  static char datePart[24];
  static char out[32];
  long long sec = unixMs / 1000;
  if (sec != cachedSec) {
    cachedSec = sec;
    formatLocal(sec * 1000, datePart, sizeof(datePart), false);
  }
  snprintf(out, sizeof(out), "%s.%03d", datePart, (int)(unixMs % 1000));
  return out;
}

void writeImuSamples() {
  if (!imuFile) return;
  static char buf[8192];
  int n = 0;
  while (tail_ != head_) {
    const Sample &s = ring[tail_ % RING];
    if (clockKnown) {
      long long u = unixAt(s.tUs);
      n += snprintf(buf + n, sizeof(buf) - n, "%.4f,%lld,%s,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f\n",
                    secsSinceStart(s.tUs), u, localStamp(u), s.ax, s.ay, s.az, s.gx, s.gy, s.gz);
    } else {
      n += snprintf(buf + n, sizeof(buf) - n, "%.4f,,,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f\n",
                    secsSinceStart(s.tUs), s.ax, s.ay, s.az, s.gx, s.gy, s.gz);
    }
    tail_ = tail_ + 1;
    samplesSaved = samplesSaved + 1;
    if (n > (int)sizeof(buf) - 120) {
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
          char line[80];
          long long u = unixAt(slotTimeUs[slot]);
          int n = clockKnown
            ? snprintf(line, sizeof(line), "%lu,%.4f,%lld,%s\n", (unsigned long)framesSaved, secsSinceStart(slotTimeUs[slot]), u, localStamp(u))
            : snprintf(line, sizeof(line), "%lu,%.4f,,\n", (unsigned long)framesSaved, secsSinceStart(slotTimeUs[slot]));
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
    if (wifiRowsReady) {   // a Wi-Fi scan finished while recording
      if (!wifiFile) {
        char path[72];
        snprintf(path, sizeof(path), "%s_wifi.csv", baseName);
        wifiFile = SD_MMC.open(path, FILE_WRITE);
        if (wifiFile) wifiFile.print("time_s,unix_ms,local_time,ssid,bssid,channel,rssi_dbm,security\n");
      }
      if (wifiFile) wifiFile.write((const uint8_t *)wifiRows, wifiRowsLen);
      wifiRowsReady = false;
    }
    if (millis() - lastFlush > 2000) {
      if (avi.f) avi.f.flush();
      if (frameFile) frameFile.flush();
      if (imuFile) imuFile.flush();
      if (wifiFile) wifiFile.flush();
      lastFlush = millis();
    }
    writerIdle = true;
  }
}

// Open the card the way Waveshare's own firmware does (1-bit, normal speed).
bool mountCard() {
  if (sdOk) return true;
  SD_MMC.end();
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT) &&
      !SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_PROBING)) {   // slow and careful, as a last try
    sdProblem = "Can't read the card. Is it FAT32? (exFAT won't work)";
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE) { SD_MMC.end(); sdProblem = "No microSD card found"; return false; }
  SD_MMC.mkdir("/rec");
  sdOk = true;
  sdProblem = "";
  sdFreeMB = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024);
  return true;
}

void startRecording() {
  if (!videoOn() && !imuOn()) { say("Video and motion are both OFF."); return; }
  if (videoOn() && !camOk) { say("Camera not found. Set video to OFF to record motion only."); return; }
  if (imuOn() && !imuOk) { say("Motion sensor not found. Set motion to OFF."); return; }
  if (!mountCard()) { say(sdProblem); return; }

  // File name: the date and time if the clock is set, otherwise a number.
  char path[72];
  if (clockKnown) {
    char when[24];
    formatLocal(nowUnixMs(), when, sizeof(when), true);
    snprintf(baseName, sizeof(baseName), "/rec/rec_%s", when);
  } else {
    for (int tries = 0; tries < 10000; tries++) {
      snprintf(baseName, sizeof(baseName), "/rec/rec_%04lu", (unsigned long)fileCounter);
      snprintf(path, sizeof(path), "%s_info.txt", baseName);
      if (!SD_MMC.exists(path)) break;
      fileCounter++;
    }
    fileCounter++;
    prefs.putUInt("n", fileCounter);
  }

  if (videoOn()) {
    snprintf(path, sizeof(path), "%s.avi", baseName);
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f || !avi.begin(f, SIZES[sizeIdx].w, SIZES[sizeIdx].h, FPS * MAX_SECONDS + FPS * 10)) {
      if (f) f.close();
      sdOk = false;
      say("Couldn't create the video file. Check the card.");
      return;
    }
    snprintf(path, sizeof(path), "%s_frames.csv", baseName);
    frameFile = SD_MMC.open(path, FILE_WRITE);
    if (frameFile) frameFile.print("frame,time_s,unix_ms,local_time\n");
  }
  if (imuOn()) {
    snprintf(path, sizeof(path), "%s_imu.csv", baseName);
    imuFile = SD_MMC.open(path, FILE_WRITE);
    if (!imuFile) {
      say("Couldn't create the motion file.");
      if (videoOn()) { avi.end(0); if (frameFile) frameFile.close(); }
      return;
    }
    imuFile.print("time_s,unix_ms,local_time,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps\n");
  }

  framesSaved = 0; framesDropped = 0; bytesSaved = 0; samplesSaved = 0; samplesDropped = 0;
  writeFailed = false;
  stopAsked = false;
  tail_ = head_;
  recStartUs = (uint32_t)esp_timer_get_time();
  recStartUnixMs = clockKnown ? nowUnixMs() : 0;
  recStartMs = millis();
  if (videoOn()) resetQuality();
  droppedAtCheck = 0;
  recording = true;
  Serial.printf("Recording %s: video %s, motion %d Hz\n", baseName, videoOn() ? SIZES[sizeIdx].name : "off",
                imuOn() ? RATES[rateIdx] : 0);
}

void writeInfoFile(float secs) {
  char path[72];
  snprintf(path, sizeof(path), "%s_info.txt", baseName);
  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) return;
  char when[32];
  if (clockKnown) {
    formatLocal(recStartUnixMs, when, sizeof(when), false);
    f.printf("started: %s (your time, UTC%+d:%02d)\n", when, tzMinutes / 60, abs(tzMinutes) % 60);
    f.printf("started_unix_ms: %lld\n", (long long)recStartUnixMs);
  } else {
    f.print("started: unknown (connect the board to the website once to set its clock)\n");
  }
  f.printf("length_s: %.2f\n", secs);
  if (videoOn())
    f.printf("video: %s, %lu frames, %.2f fps achieved, %lu frames lost, %lu damaged frames skipped since power-on\n",
             SIZES[sizeIdx].name, (unsigned long)framesSaved, secs > 0 ? framesSaved / secs : 0,
             (unsigned long)framesDropped, (unsigned long)damagedFrames);
  else
    f.print("video: off\n");
  if (imuOn())
    f.printf("motion: %d Hz, %lu rows, %lu lost; accel +/-8 g, gyro +/-1024 deg/s\n", RATES[rateIdx],
             (unsigned long)samplesSaved, (unsigned long)samplesDropped);
  else
    f.print("motion: off\n");
  f.printf("firmware: %s\n", FIRMWARE_BUILD);
  f.close();
}

void stopRecording() {
  stopAsked = true;
  uint32_t elapsedUs = (uint32_t)esp_timer_get_time() - recStartUs;
  uint32_t t0 = millis();
  while ((uxQueueMessagesWaiting(fullSlots) > 0 || tail_ != head_) && millis() - t0 < 10000) delay(10);
  while (!writerIdle && millis() - t0 < 12000) delay(5);
  recording = false;
  delay(30);
  while (!writerIdle) delay(5);
  if (videoOn()) {
    avi.end(framesSaved ? elapsedUs : 0);   // stamped with the real frame rate
    if (frameFile) frameFile.close();
  }
  if (imuFile) imuFile.close();
  if (wifiFile) wifiFile.close();
  wifiRowsReady = false;
  float secs = elapsedUs / 1e6f;
  writeInfoFile(secs);
  snprintf(lastSaved, sizeof(lastSaved), "%s  (%.0f s)", baseName + 5, secs);
  sdFreeMB = (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024 * 1024);
  Serial.printf("Saved %s: %lu frames (%lu lost), %lu motion rows (%lu lost), %.1f s\n", baseName,
                (unsigned long)framesSaved, (unsigned long)framesDropped, (unsigned long)samplesSaved,
                (unsigned long)samplesDropped, secs);
  say(writeFailed ? "Stopped: the card is full or was removed." : "Saved!");
}

// ============================================================ drawing

const uint16_t BG = 0x10A3, PANEL = 0x18E5, INK = 0xFFFF, MUTED = 0xAD55;
const uint16_t AXIS_COL[3] = {0xF9A6, 0x5F2B, 0x4D9F};   // x red, y green, z blue
int previewX = 0, previewY = 0;
const int HIST = 150;
float hist[6][HIST];   // accel x, y, z, then rotation x, y, z
int histN = 0;

void textAt(Arduino_GFX *g, const GFXfont *f, int x, int y, const char *s, uint16_t color) {
  g->setFont(f); g->setTextSize(1); g->setTextColor(color); g->setCursor(x, y); g->print(s);
  g->setFont((const GFXfont *)NULL);
}
int textWidth(Arduino_GFX *g, const GFXfont *f, const char *s) {
  int16_t x1, y1; uint16_t w, h;
  g->setFont(f); g->getTextBounds(s, 0, 0, &x1, &y1, &w, &h); g->setFont((const GFXfont *)NULL);
  return w;
}
void small(Arduino_GFX *g, int x, int y, const char *s, uint16_t color) {
  g->setFont((const GFXfont *)NULL); g->setTextSize(1); g->setTextColor(color); g->setCursor(x, y); g->print(s);
}

// ---- Camera preview ----
// A decode task on the other core turns each camera picture into pixels in one of
// two buffers, while the main loop sends the previous finished picture to the
// screen in one go. So decoding and drawing happen at the same time, and you
// never see a picture being painted in strips.
enum : uint8_t { BUF_FREE, BUF_READY, BUF_SHOWING };
uint16_t *frameBufs[2] = {nullptr, nullptr};   // up to 320 x 240 pixels each, in PSRAM
volatile uint8_t bufState[2] = {BUF_FREE, BUF_FREE};
int bufW[2], bufH[2], bufX[2], bufY[2];
uint32_t bufSeq[2];
uint32_t decodeSeq = 0;
portMUX_TYPE bufLock = portMUX_INITIALIZER_UNLOCKED;
JPEGDEC jpegDec;                 // used only by the decode task
uint16_t *decodeTarget = nullptr;
int decodeW = 0, decodeH = 0;

int drawJpegBlock(JPEGDRAW *d) {
  for (int row = 0; row < d->iHeight; row++) {
    int y = d->y + row;
    if (y >= decodeH) break;
    int w = min((int)d->iWidth, decodeW - d->x);
    if (w > 0) memcpy(decodeTarget + (size_t)y * decodeW + d->x, d->pPixels + (size_t)row * d->iWidth, w * 2);
  }
  return 1;
}

void decodeTask(void *) {
  while (true) {
    if (!camOk || !videoOn() || previewWanted || !previewLen) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
    // Find a buffer that isn't on its way to the screen.
    int b = -1;
    portENTER_CRITICAL(&bufLock);
    for (int i = 0; i < 2 && b < 0; i++) if (bufState[i] == BUF_FREE) b = i;
    if (b < 0) for (int i = 0; i < 2 && b < 0; i++) if (bufState[i] == BUF_READY) b = i;   // replace an older unshown one
    if (b >= 0) bufState[b] = BUF_SHOWING;   // reserved while we write it
    portEXIT_CRITICAL(&bufLock);
    if (b < 0) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }

    const Size &s = SIZES[sizeIdx];
    int div = s.scale == JPEG_SCALE_HALF ? 2 : 4;
    decodeW = min(s.w / div, SCREEN_W);
    decodeH = min(s.h / div, PREV_H);
    decodeTarget = frameBufs[b];
    bool ok = false;
    if (jpegDec.openRAM(previewBuf, previewLen, drawJpegBlock)) {
      jpegDec.setPixelType(RGB565_LITTLE_ENDIAN);
      ok = jpegDec.decode(0, 0, s.scale);
      jpegDec.close();
    }
    previewWanted = true;   // ask the camera task for the next picture
    bufW[b] = decodeW; bufH[b] = decodeH;
    bufX[b] = (SCREEN_W - decodeW) / 2;
    bufY[b] = PREV_Y + (PREV_H - decodeH) / 2;
    bufSeq[b] = ++decodeSeq;
    portENTER_CRITICAL(&bufLock);
    bufState[b] = ok ? BUF_READY : BUF_FREE;
    portEXIT_CRITICAL(&bufLock);
  }
}

void drawRecTag() {
  bool blink = (millis() / 500) % 2;
  lcd->drawRect(0, PREV_Y, SCREEN_W, PREV_H, rgb(230, 50, 50));
  lcd->drawRect(1, PREV_Y + 1, SCREEN_W - 2, PREV_H - 2, rgb(230, 50, 50));
  lcd->fillRoundRect(8, PREV_Y + 8, 58, 22, 6, rgb(0, 0, 0));
  lcd->fillCircle(20, PREV_Y + 19, 5, blink ? rgb(240, 50, 50) : rgb(100, 30, 30));
  small(lcd, 30, PREV_Y + 15, "REC", INK);
}

// Sends the newest finished picture to the screen, if there is one.
bool updatePreview() {
  if (!camOk || !videoOn()) return false;
  int b = -1;
  portENTER_CRITICAL(&bufLock);
  for (int i = 0; i < 2; i++)
    if (bufState[i] == BUF_READY && (b < 0 || bufSeq[i] > bufSeq[b])) b = i;
  if (b >= 0) bufState[b] = BUF_SHOWING;
  portEXIT_CRITICAL(&bufLock);
  if (b < 0) return false;
  lcd->draw16bitRGBBitmap(bufX[b], bufY[b], frameBufs[b], bufW[b], bufH[b]);   // the whole picture at once
  if (recording) drawRecTag();
  portENTER_CRITICAL(&bufLock);
  bufState[b] = BUF_FREE;
  portEXIT_CRITICAL(&bufLock);
  previewFrames++;
  return true;
}

void clearPreview(const char *why, uint16_t color) {
  lcd->fillRect(0, PREV_Y, SCREEN_W, PREV_H, 0x0000);
  int w = why ? textWidth(lcd, &FreeSans9pt7b, why) : 0;
  if (why) textAt(lcd, &FreeSans9pt7b, (SCREEN_W - w) / 2, PREV_Y + 125, why, color);
}

// ---- Header ----
void drawHeader() {
  head->fillScreen(BG);
  textAt(head, &FreeSansBold12pt7b, 12, 30, "Recorder", INK);
  char buf[64];
  if (sdOk) snprintf(buf, sizeof(buf), "SD: %.1f GB free", sdFreeMB / 1024.0);
  else snprintf(buf, sizeof(buf), "%s", sdProblem[0] ? "SD card problem" : "no SD card");
  small(head, 320 - 8 - strlen(buf) * 6, 12, buf, sdOk ? MUTED : rgb(255, 120, 110));
  if (clockKnown) formatLocal(nowUnixMs(), buf, sizeof(buf), false);
  else snprintf(buf, sizeof(buf), "clock not set");
  small(head, 320 - 8 - strlen(buf) * 6, 26, buf, clockKnown ? MUTED : rgb(255, 200, 120));
  head->flush();
}

// ---- Controls area (graph, status, options, REC) ----
void drawLow() {
  low->fillScreen(BG);
  char buf[96];

  // Motion graph
  low->fillRoundRect(8, GRAPH_Y, 304, GRAPH_H, 8, PANEL);
  if (!imuOk) {
    small(low, 18, GRAPH_Y + 20, "Motion sensor not found", rgb(255, 120, 110));
  } else {
    Sample s;
    portENTER_CRITICAL(&latestLock);
    s = latest;
    portEXIT_CRITICAL(&latestLock);
    // Two small graphs side by side: acceleration (left, +/-2 g) and rotation
    // (right, +/-250 deg/s). x red, y green, z blue.
    const char *titles[2] = {"accel (g)", "rotation (deg/s)"};
    const float ranges[2] = {2.0f, 250.0f};
    for (int g = 0; g < 2; g++) {
      int gx = 14 + g * 150, gw = 142, gy = GRAPH_Y + 14, gh = GRAPH_H - 18, mid = gy + gh / 2;
      small(low, gx, GRAPH_Y + 4, titles[g], MUTED);
      low->drawFastHLine(gx, mid, gw, 0x39C7);
      for (int a = 0; a < 3 && histN > 1; a++) {
        int px = 0, py = 0;
        for (int i = 0; i < histN; i++) {
          float v = constrain(hist[g * 3 + a][i], -ranges[g], ranges[g]);
          int xx = gx + i * gw / (HIST - 1), yy = mid - (int)(v / ranges[g] * (gh / 2 - 1));
          if (i) low->drawLine(px, py, xx, yy, AXIS_COL[a]);
          px = xx; py = yy;
        }
      }
    }
  }

  // Status
  low->fillRoundRect(8, STAT_Y, 304, 36, 8, PANEL);
  if (recording) {
    float secs = (millis() - recStartMs) / 1000.0f;
    snprintf(buf, sizeof(buf), "%d:%02d", (int)secs / 60, (int)secs % 60);
    textAt(low, &FreeSansBold9pt7b, 16, STAT_Y + 22, buf, INK);
    if (videoOn())
      snprintf(buf, sizeof(buf), "%.1f fps  %.1f MB  Q%d  lost %lu", secs > 0.5f ? framesSaved / secs : 0.0f,
               bytesSaved / 1048576.0f, quality, (unsigned long)framesDropped);
    else
      snprintf(buf, sizeof(buf), "video off");
    small(low, 76, STAT_Y + 7, buf, framesDropped ? rgb(255, 170, 120) : rgb(140, 230, 160));
    if (imuOn())
      snprintf(buf, sizeof(buf), "%lu motion rows  lost %lu", (unsigned long)(samplesSaved + (head_ - tail_)),
               (unsigned long)samplesDropped);
    else
      snprintf(buf, sizeof(buf), "motion off");
    small(low, 76, STAT_Y + 21, buf, samplesDropped ? rgb(255, 170, 120) : rgb(140, 230, 160));
  } else if (millis() < messageUntil) {
    small(low, 16, STAT_Y + 14, message, rgb(255, 210, 120));
  } else if (!sdOk) {
    small(low, 16, STAT_Y + 7, sdProblem[0] ? sdProblem : "Put a microSD card in.", rgb(255, 170, 120));
    small(low, 16, STAT_Y + 21, "It's checked again every few seconds.", MUTED);
  } else {
    if (lastSaved[0]) snprintf(buf, sizeof(buf), "Last: %s", lastSaved);
    else snprintf(buf, sizeof(buf), "Ready. Tap REC to record.");
    small(low, 16, STAT_Y + 7, buf, MUTED);
    if (videoOn() && camOk)
      snprintf(buf, sizeof(buf), "camera %.0f fps  screen %.0f  Q%d  damaged %lu", cameraFps, previewFps, quality,
               (unsigned long)damagedFrames);
    else
      snprintf(buf, sizeof(buf), "camera off");
    small(low, 16, STAT_Y + 21, buf, MUTED);
  }

  // Options: tap to change
  const char *vid = videoOn() ? SIZES[sizeIdx].name : "OFF";
  char mot[12];
  if (imuOn()) snprintf(mot, sizeof(mot), "%d Hz", RATES[rateIdx]);
  else snprintf(mot, sizeof(mot), "OFF");
  char wifiVal[12];
  if (wifiOn) snprintf(wifiVal, sizeof(wifiVal), "%d", netCount);
  else snprintf(wifiVal, sizeof(wifiVal), "OFF");
  const char *labels[4] = {"Video", "Motion", "Flip", wifiOn ? "Wi-Fi nets" : "Wi-Fi"};
  const char *values[4] = {vid, mot, "turn", wifiVal};
  for (int i = 0; i < 4; i++) {
    int x = 8 + i * 77, w = 72;
    bool locked = recording && i < 3;   // Wi-Fi can be switched any time
    bool lit = i == 3 && wifiOn;
    low->fillRoundRect(x, OPT_Y, w, 36, 8, locked ? BG : (lit ? rgb(40, 90, 150) : PANEL));
    small(low, x + 6, OPT_Y + 5, labels[i], MUTED);
    textAt(low, &FreeSans9pt7b, x + 6, OPT_Y + 30, values[i], locked ? 0x8410 : INK);
  }

  // REC / STOP
  low->fillRoundRect(8, REC_Y, 304, 46, 12, recording ? rgb(80, 80, 96) : rgb(220, 50, 60));
  if (recording) low->fillRect(122, REC_Y + 14, 18, 18, INK);
  else low->fillCircle(130, REC_Y + 23, 10, INK);
  textAt(low, &FreeSansBold12pt7b, 150, REC_Y + 32, recording ? "STOP" : "REC", INK);
  low->flush();
}

// ============================================================ touch and commands

bool wasDown = false;
bool previewNeedsClear = true;

void nextVideoSize() {
  int next = (sizeIdx + 1) % (NUM_SIZES + 1);   // ... 1280x720, OFF, 640x480 ...
  if (next < NUM_SIZES && camOk) {
    sensor_t *s = esp_camera_sensor_get();
    if (!s || s->set_framesize(s, SIZES[next].fs) != 0) return;
  }
  sizeIdx = next;
  prefs.putInt("size", sizeIdx);
  if (videoOn()) resetQuality();
  previewWanted = true;
  previewNeedsClear = true;
}

void nextMotionRate() {
  rateIdx = (rateIdx + 1) % (NUM_RATES + 1);    // ... 200 Hz, OFF, 25 Hz ...
  imuHz = imuOn() ? RATES[rateIdx] : 50;        // still read at 50 Hz for the live graph
  prefs.putInt("rate", rateIdx);
}

void nextOrientation() {
  orient = (orient + 1) % 4;
  prefs.putInt("orient", orient);
  applyOrientation();
}

void handleTouch() {
  if (!touchOk) return;
  int16_t xs[1], ys[1];
  bool down = touch.getPoint(xs, ys, 1) > 0;
  if (down && !wasDown) {
    int x = xs[0], y = ys[0] - LOW_Y;
    if (y >= REC_Y && y <= REC_Y + 46) {
      if (recording) stopRecording();
      else startRecording();
    } else if (y >= OPT_Y && y <= OPT_Y + 36) {
      int i = (x - 8) / 77;
      if (i == 3) { wifiSet(!wifiOn); if (!wifiOn) previewNeedsClear = true; }   // any time, even while recording
      else if (!recording) {
        if (i <= 0) nextVideoSize();
        else if (i == 1) nextMotionRate();
        else nextOrientation();
      }
    }
  }
  wasDown = down;
}

char cmdBuf[96];
int cmdLen = 0;

void readCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      cmdBuf[cmdLen] = 0;
      cmdLen = 0;
      long long ms = 0;
      int tz = 0;
      if (sscanf(cmdBuf, "settime %lld %d", &ms, &tz) >= 1 && ms > 1700000000000LL) setClock(ms, tz);
      else if (!strcmp(cmdBuf, "version"))
        Serial.printf("{\"t\":\"hi\",\"app\":\"recorder\",\"build\":\"%s\"}\n", FIRMWARE_BUILD);
    } else if (cmdLen < (int)sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = c;
    }
  }
}

// ============================================================ setup & loop

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  BoardStatus st = boardPowerUp(expander, pmu);   // also powers the camera (power chip BLDO1/BLDO2)

  bool lcdOk = lcd->begin(LCD_SPI_HZ, SPI_MODE0);
  screenOk = lcdOk && head->begin(GFX_SKIP_OUTPUT_BEGIN) && low->begin(GFX_SKIP_OUTPUT_BEGIN);
  if (lcdOk) lcd->fillScreen(BG);
  touchOk = touch.begin(Wire, FT6X36_SLAVE_ADDRESS);
  rtcOk = rtc.begin(Wire);

  imuOk = imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS);
  if (imuOk) {
    imu.configAccelerometer(SensorQMI8658::ACC_RANGE_8G, SensorQMI8658::ACC_ODR_500Hz, SensorQMI8658::LPF_MODE_3);
    imu.configGyroscope(SensorQMI8658::GYR_RANGE_1024DPS, SensorQMI8658::GYR_ODR_448_4Hz, SensorQMI8658::LPF_MODE_3);
    imu.enableAccelerometer();
    imu.enableGyroscope();
  }

  prefs.begin("recorder", false);
  fileCounter = prefs.getUInt("n", 1);
  sizeIdx = constrain(prefs.getInt("size", 0), 0, NUM_SIZES);
  rateIdx = constrain(prefs.getInt("rate", 2), 0, NUM_RATES);
  orient = constrain(prefs.getInt("orient", 0), 0, 3);
  tzMinutes = prefs.getInt("tz", 0);
  imuHz = imuOn() ? RATES[rateIdx] : 50;
  readClockChip();

  slotMem = (uint8_t *)ps_malloc((size_t)SLOTS * SLOT_BYTES);
  previewBuf = (uint8_t *)ps_malloc(SLOT_BYTES);
  frameBufs[0] = (uint16_t *)ps_malloc(SCREEN_W * PREV_H * 2);
  frameBufs[1] = (uint16_t *)ps_malloc(SCREEN_W * PREV_H * 2);
  ring = (Sample *)ps_malloc(RING * sizeof(Sample));
  freeSlots = xQueueCreate(SLOTS, sizeof(int));
  fullSlots = xQueueCreate(SLOTS, sizeof(int));
  for (int i = 0; slotMem && i < SLOTS; i++) xQueueSend(freeSlots, &i, 0);
  if (!ring) imuOk = false;

  camOk = slotMem && previewBuf && startCamera();
  mountCard();

  Serial.printf("[boot] Recorder build %s | PSRAM %s (%u KB free) | camera %s | motion sensor %s | clock %s | touch %s | SD %s\n",
                FIRMWARE_BUILD, st.psram ? "found" : "NOT FOUND", (unsigned)(ESP.getFreePsram() / 1024),
                camOk ? "ok" : "FAILED", imuOk ? "ok" : "MISSING", clockKnown ? "set" : (rtcOk ? "not set" : "MISSING"),
                touchOk ? "ok" : "MISSING", sdOk ? "ok" : sdProblem);

  if (imuOk) xTaskCreatePinnedToCore(imuTask, "imu", 4096, nullptr, configMAX_PRIORITIES - 1, nullptr, 0);
  xTaskCreatePinnedToCore(captureTask, "capture", 4096, nullptr, configMAX_PRIORITIES - 2, nullptr, 0);
  xTaskCreatePinnedToCore(writerTask, "writer", 8192, nullptr, configMAX_PRIORITIES - 3, nullptr, 1);
  // Preview decoding: on the camera's core, below the camera and motion tasks so it
  // never delays a recording; it uses whatever time is left over.
  if (camOk && frameBufs[0] && frameBufs[1])
    xTaskCreatePinnedToCore(decodeTask, "decode", 8192, nullptr, 2, nullptr, 0);
  if (prefs.getBool("wifi", false)) wifiSet(true);   // it was scanning last time
}

// The Wi-Fi list, in place of the camera picture while scanning is on.
void drawWifiList() {
  lcd->fillRect(0, PREV_Y, SCREEN_W, PREV_H, 0x0000);
  char buf[64];
  if (wifiScans == 0) {
    textAt(lcd, &FreeSans9pt7b, 70, PREV_Y + 125, "Scanning for Wi-Fi...", MUTED);
    return;
  }
  char when[32] = "";
  if (wifiLastScanUnixMs) formatLocal(wifiLastScanUnixMs, when, sizeof(when), false);
  snprintf(buf, sizeof(buf), "%d networks   %s", netCount, when[0] ? when + 11 : "");
  small(lcd, 8, PREV_Y + 6, buf, MUTED);
  small(lcd, 250, PREV_Y + 6, "ch   dBm", MUTED);
  int rows = min(netCount, 10);
  for (int i = 0; i < rows; i++) {
    const Net &w = nets[i];
    int y = PREV_Y + 20 + i * 22;
    // Signal bars: 4 = strong (-55 dBm or better) ... 1 = weak
    int bars = w.rssi >= -55 ? 4 : w.rssi >= -67 ? 3 : w.rssi >= -78 ? 2 : 1;
    for (int b = 0; b < 4; b++)
      lcd->fillRect(8 + b * 5, y + 14 - (b + 1) * 3, 3, (b + 1) * 3, b < bars ? rgb(120, 220, 150) : 0x3186);
    char name[26];
    snprintf(name, sizeof(name), "%s", w.ssid);
    textAt(lcd, &FreeSans9pt7b, 34, y + 14, name, INK);
    snprintf(buf, sizeof(buf), "%2d  %4d", w.ch, w.rssi);
    small(lcd, 250, y + 6, buf, MUTED);
    if (w.open) small(lcd, 214, y + 6, "open", rgb(255, 200, 120));
  }
  if (netCount > rows) {
    snprintf(buf, sizeof(buf), "+ %d more (see the website)", netCount - rows);
    small(lcd, 8, PREV_Y + PREV_H - 10, buf, MUTED);
  }
}

uint32_t lastUi = 0, lastGraph = 0, lastCardCheck = 0;

void loop() {
  handleTouch();
  readCommands();
  if (recording && (writeFailed || (millis() - recStartMs) / 1000 >= MAX_SECONDS)) stopRecording();
  autoQuality();

  // Live motion readings for the website, 20 times a second (only while connected).
  static uint32_t lastStream = 0;
  if (imuOk && Serial && millis() - lastStream >= 50) {
    lastStream = millis();
    Sample s;
    portENTER_CRITICAL(&latestLock);
    s = latest;
    portEXIT_CRITICAL(&latestLock);
    Serial.printf("{\"t\":\"imu\",\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,\"gx\":%.1f,\"gy\":%.1f,\"gz\":%.1f}\n",
                  s.ax, s.ay, s.az, s.gx, s.gy, s.gz);
  }
  if (!sdOk && !recording && millis() - lastCardCheck > 4000) { lastCardCheck = millis(); mountCard(); }
  wifiPoll();
  if (!screenOk) { delay(5); return; }

  if (wifiOn) {
    // Wi-Fi list in place of the camera picture (recording carries on underneath)
    if (wifiListDirty) { wifiListDirty = false; drawWifiList(); }
  } else {
    // Camera preview: as fast as pictures arrive (up to 20 a second)
    if (previewNeedsClear) {
      previewNeedsClear = false;
      if (!camOk) clearPreview("Camera not found", rgb(255, 120, 110));
      else if (!videoOn()) clearPreview("Video is OFF", MUTED);
      else clearPreview(nullptr, 0);
    }
    updatePreview();
  }

  // Motion graph history: 10 points a second
  if (millis() - lastGraph >= 100) {
    lastGraph = millis();
    Sample s;
    portENTER_CRITICAL(&latestLock);
    s = latest;
    portEXIT_CRITICAL(&latestLock);
    if (histN == HIST) {
      for (int a = 0; a < 6; a++) memmove(hist[a], hist[a] + 1, sizeof(float) * (HIST - 1));
      histN--;
    }
    hist[0][histN] = s.ax; hist[1][histN] = s.ay; hist[2][histN] = s.az;
    hist[3][histN] = s.gx; hist[4][histN] = s.gy; hist[5][histN] = s.gz;
    histN++;
  }

  // Header and controls: 4 times a second (instantly after a tap)
  static bool lastDown = false;
  if (millis() - lastUi >= 250 || wasDown != lastDown) {
    lastDown = wasDown;
    lastUi = millis();
    if (millis() - previewFpsAt >= 1000) {
      previewFps = previewFrames * 1000.0f / (millis() - previewFpsAt);
      uint32_t cf = cameraFrames;
      cameraFps = (cf - cameraFramesAt) * 1000.0f / (millis() - previewFpsAt);
      cameraFramesAt = cf;
      previewFrames = 0;
      previewFpsAt = millis();
    }
    drawHeader();
    drawLow();
    static bool backlightOn = false;
    if (!backlightOn) { boardBacklight(true); backlightOn = true; }
  }
  delay(1);
}
