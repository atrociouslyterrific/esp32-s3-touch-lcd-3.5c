// board.h: power-up for the Waveshare ESP32-S3-Touch-LCD-3.5-C.
//
// Follows the same order as Waveshare's factory firmware:
//   1. backlight off (so nothing flashes while the screen wakes up)
//   2. IO expander: pulse the LCD reset (EXIO1), switch on EXIO7
//   3. AXP2101 power chip: set and switch on its power rails
//   4. after a cold power-on, restart once (the screen doesn't start
//      reliably on the very first boot; Waveshare does the same)
// The backlight is switched on later, after the first frame is drawn.
#pragma once
#include <Wire.h>
#include <TCA9554.h>
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>
#include <esp_system.h>

#define LCD_BL 6
#define SPI_MISO 2
#define SPI_MOSI 1
#define SPI_SCLK 5
#define LCD_DC 3
#define I2C_SDA 8
#define I2C_SCL 7
#define EXP_LCD_RST 1
#define EXP_PWR_EN 7

struct BoardStatus {
  bool expander, pmu, psram;
  bool coldRestart;          // true if this boot came from the cold-start restart
  esp_reset_reason_t reason;
};

inline const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "CRASH (panic)";
    case ESP_RST_INT_WDT: return "CRASH (interrupt watchdog)";
    case ESP_RST_TASK_WDT: return "CRASH (task watchdog)";
    case ESP_RST_WDT: return "CRASH (watchdog)";
    case ESP_RST_BROWNOUT: return "brownout (power dipped)";
    case ESP_RST_USB: return "USB reset";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    default: return "other";
  }
}

// Survives a software restart, so we only do the cold-start restart once.
RTC_NOINIT_ATTR uint32_t boardColdRestartMagic;

inline BoardStatus boardPowerUp(TCA9554 &expander, XPowersPMU &pmu) {
  BoardStatus st = {};
  st.reason = esp_reset_reason();
  st.psram = psramFound();

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, LOW);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  st.expander = expander.begin();
  expander.pinMode1(EXP_LCD_RST, OUTPUT);
  expander.pinMode1(EXP_PWR_EN, OUTPUT);
  expander.write1(EXP_LCD_RST, 0);
  delay(100);
  expander.write1(EXP_LCD_RST, 1);
  expander.write1(EXP_PWR_EN, 1);
  delay(100);

  st.pmu = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA, I2C_SCL);
  if (st.pmu) {
    pmu.setVbusVoltageLimit(XPOWERS_AXP2101_VBUS_VOL_LIM_4V36);
    pmu.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_1500MA);
    pmu.setSysPowerDownVoltage(2600);
    pmu.setDC1Voltage(3300);
    pmu.setDC3Voltage(3300);
    pmu.setDC5Voltage(3300);
    pmu.setALDO1Voltage(3300);
    pmu.setALDO2Voltage(3300);
    pmu.setALDO3Voltage(3300);
    pmu.setALDO4Voltage(3300);
    pmu.setBLDO1Voltage(1500);
    pmu.setBLDO2Voltage(2800);
    pmu.setDLDO1Voltage(3300);
    pmu.enableDC3(); pmu.enableDC5();
    pmu.enableALDO1(); pmu.enableALDO2(); pmu.enableALDO3(); pmu.enableALDO4();
    pmu.enableBLDO1(); pmu.enableBLDO2();
    pmu.enableDLDO1();
    pmu.settDC1WorkModeToPwm(1);   // Waveshare: stops backlight flicker at low brightness
    pmu.disableTSPinMeasure();
    pmu.enableBattDetection();
    pmu.enableVbusVoltageMeasure();
    pmu.enableBattVoltageMeasure();
    pmu.setChargingLedMode(XPOWERS_CHG_LED_CTRL_CHG);
    delay(20);
  }

  // Cold-start workaround: restart once after a power-on.
  if (st.reason == ESP_RST_POWERON && boardColdRestartMagic != 0xB100B) {
    boardColdRestartMagic = 0xB100B;
    delay(50);
    esp_restart();
  }
  st.coldRestart = (boardColdRestartMagic == 0xB100B);
  boardColdRestartMagic = 0;
  return st;
}

inline void boardBacklight(bool on) {
  digitalWrite(LCD_BL, on ? HIGH : LOW);
}
