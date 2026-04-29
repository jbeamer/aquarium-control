#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_HX8357.h>
#include <Adafruit_TSC2007.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "wifi_ota.h"
#include "sd_logger.h"
#include "web_ui.h"

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

// ---------------------------------------------------------------------------
// Pin definitions
//
// TFT FeatherWing (fixed): TFT_CS=15, TFT_DC=33, TSC2007 touch over I2C.
// Relays: active-HIGH. Filter is normally-closed (LOW=on, HIGH=off).
// Current sensors: ADC1 pins only (readable while WiFi is active).
// ---------------------------------------------------------------------------
#define TFT_CS            15
#define TFT_DC            33
#define TEMP_SENSOR_PIN   26   // moved from GPIO32 to free SD_CS

#define LIGHTS_RELAY_PIN  13   // active HIGH (also onboard red LED)
#define HEATER_RELAY_PIN  12   // active HIGH (GPIO 12 strap pin: must be LOW at boot)
#define FILTER_RELAY_PIN  27   // normally-closed: LOW=on, HIGH=off

#define LIGHTS_SENSE_PIN  34   // ADC1 ch6, input-only
#define HEATER_SENSE_PIN  39   // ADC1 ch3, input-only
#define FILTER_SENSE_PIN  36   // ADC1 ch0, input-only
#define TFT_LITE_PIN      25   // A1 — TFT backlight, HIGH=on LOW=off
#define SD_CS_PIN         14   // FeatherWing SD card CS

// ---------------------------------------------------------------------------
// Colors — flat dark scheme
// ---------------------------------------------------------------------------
#define C_BG       0x0841   // very dark blue-gray
#define C_PANEL    0x18C3   // dark panel (temp side)
#define C_HEADER   0x1082   // header and sensor-row bar
#define C_DIVIDER  0x4A69   // panel border lines
#define C_WHITE    0xFFFF
#define C_GRAY     0x8410
#define C_DIM      0x39E7   // inactive / off state
#define C_GREEN    0x07E0
#define C_AMBER    0xFD20
#define C_RED      0xF800
#define C_YELLOW   0xFFE0
#define C_BLUE_DIM 0x2965   // water-change button normal state
#define C_BLUE     0x4ABF   // heater-off indicator (cool blue)

// ---------------------------------------------------------------------------
// Layout constants (absolute screen coords)
// Screen: 480 wide × 320 tall, setRotation(1)
// ---------------------------------------------------------------------------
#define SCREEN_W   480
#define SCREEN_H   320
#define HEADER_H    40
#define SENSOR_H    40
#define SPLIT_X    240          // x boundary between temp and lights panels
#define MAIN_Y     HEADER_H     // 40
#define MAIN_H     (SCREEN_H - HEADER_H - SENSOR_H)  // 240
#define SENSOR_Y   (SCREEN_H - SENSOR_H)             // 280

// Water-change button: lives in the right (lights) panel, bottom section
#define WCHG_X     (SPLIT_X + 12)
#define WCHG_Y     (MAIN_Y + 180)
#define WCHG_W     (SCREEN_W - SPLIT_X - 24)  // 216px wide
#define WCHG_H     52

// Lights pill / state zone — LPILL_X/W shared by all three light states
#define LPILL_X    WCHG_X
#define LPILL_W    WCHG_W

// Override-ON state: amber pill + countdown geometry
#define LOVRD_PILL_Y   (MAIN_Y + 60)
#define LOVRD_PILL_H   44
#define LOVRD_CD_Y     (LOVRD_PILL_Y + LOVRD_PILL_H + 22)  // countdown text baseline

// ---------------------------------------------------------------------------
// Lights schedule — NTP-based, Pacific time with DST
// ---------------------------------------------------------------------------
#define TZ_PACIFIC       "PST8PDT,M3.2.0,M11.1.0"

// ---------------------------------------------------------------------------
// Thermostat config
// ---------------------------------------------------------------------------
#define MAINT_EXIT_MIN_TEMP 74.0f   // min temp to allow exiting water-change mode

// Aliases — all tunable values live in WebUi and persist via Preferences.
#define SET_POINT      WebUi::setPoint
#define HEATER_HYST    WebUi::heaterHyst
#define THRESH_LIGHTS  WebUi::threshLights
#define THRESH_HEATER  WebUi::threshHeater
#define THRESH_FILTER  WebUi::threshFilter

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
#define TEMP_INTERVAL     2000UL
#define SENSOR_INTERVAL   2000UL
#define SLEEP_MS      (5UL * 60 * 1000)
#define OVERRIDE_MS   (5UL * 60 * 1000)
#define CONFIRM_MS    2000UL

// Current sensor sampling: 200 samples at ~160µs each ≈ 32ms ≈ 1.9 AC cycles.
// A single analogRead() only catches one arbitrary phase of the 60 Hz waveform,
// making AC current invisible. Rapid multi-sample + peak-to-peak reveals it.
#define CURR_SAMPLES  200
#define CURR_DELAY_US 100   // µs delay added after each ~60µs analogRead
#define CURR_HIST_MAX 30    // max averaging window (compile-time array bound)

// ---------------------------------------------------------------------------
// Hardware objects
// ---------------------------------------------------------------------------
Adafruit_HX8357 tft(TFT_CS, TFT_DC);
Adafruit_TSC2007 touch;
OneWire oneWire(TEMP_SENSOR_PIN);
DallasTemperature tempSensor(&oneWire);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
float currentTemp = 74.0f;
unsigned long lastTempRead = 0;

bool heaterOn = false;
bool lightsOn = false;
bool filterOn = true;

// Lights 5-minute manual override
bool lightsOverride = false;
unsigned long lightsOverrideStart = 0;
unsigned long lastCountdownDraw = 0;

// Water-change / maintenance mode
enum MaintState : uint8_t { MAINT_NORMAL, MAINT_CONFIRM, MAINT_ACTIVE, MAINT_DONE_CONFIRM };
MaintState maintState = MAINT_NORMAL;
unsigned long maintConfirmAt = 0;

// Display sleep
bool displayAsleep = false;
unsigned long lastActivity = 0;

// Temp sensor validity — heater is forced off when false
bool tempValid = false;

// Current sensor readings — peak-to-peak ADC counts over one sample burst.
// At 0 current the AC waveform amplitude is ~0, so pkpk ≈ noise floor.
// With current flowing, pkpk rises proportionally to load.
int pkpkLights = 0, pkpkHeater = 0, pkpkFilter = 0;
unsigned long lastSensorRead = 0;

// Averaging ring buffers for fault detection (window = WebUi::currAvgWindow samples)
int currHistLights[CURR_HIST_MAX] = {};
int currHistHeater[CURR_HIST_MAX] = {};
int currHistFilter[CURR_HIST_MAX] = {};
int currHistHead  = 0;
int currHistCount = 0;
int avgPkPkLights = 0, avgPkPkHeater = 0, avgPkPkFilter = 0;

// Circular temperature history — 180 averaged samples at 10s each = 30 min window.
float tempHistory[180];
int   tempHistHead  = 0;
int   tempHistCount = 0;
float tempAccum     = 0.0f;
int   tempAccumN    = 0;

void updateLightsStatus();
void updateClockDisplay();
void updateHeaterBadge();
void drawSparkline();
void drawWaterChangeReminder();

// ---------------------------------------------------------------------------
// Schedule helpers
// ---------------------------------------------------------------------------
// Returns true if current time is within the lights schedule window.
// Returns 'fallback' if the RTC/NTP time is unavailable, so callers can
// preserve the last known state instead of defaulting to off.
static bool isScheduleTime(bool fallback) {
  struct tm t;
  if (!getLocalTime(&t, 0)) return fallback;
  if (t.tm_year + 1900 < 2024) return fallback;  // NTP not yet synced
  return t.tm_hour >= WebUi::lightsOnHour && t.tm_hour < WebUi::lightsOffHour;
}

// Returns "Next: ON at 9am" / "Next: OFF at 3pm" based on current schedule.
static void nextScheduleStr(char* buf, int len) {
  struct tm t;
  if (!getLocalTime(&t, 0) || t.tm_year + 1900 < 2024) {
    snprintf(buf, len, ""); return;
  }
  bool inSched = (t.tm_hour >= WebUi::lightsOnHour && t.tm_hour < WebUi::lightsOffHour);
  int h = inSched ? WebUi::lightsOffHour : WebUi::lightsOnHour;
  int h12 = h % 12; if (h12 == 0) h12 = 12;
  const char* ap = (h < 12) ? "am" : "pm";
  snprintf(buf, len, inSched ? "Next: OFF at %d%s" : "Next: ON at %d%s", h12, ap);
}

// ---------------------------------------------------------------------------
// Relay helpers — idempotent, log on change
// ---------------------------------------------------------------------------
void setHeater(bool on) {
  if (heaterOn == on) return;
  heaterOn = on;
  digitalWrite(HEATER_RELAY_PIN, on ? HIGH : LOW);
  WifiOta::logf("[Heater] %s\n", on ? "ON" : "OFF");
  time_t ts; time(&ts);
  SdLogger::logEvent(ts, on ? "Heater ON" : "Heater OFF");
}

void setLights(bool on) {
  if (lightsOn == on) return;
  lightsOn = on;
  digitalWrite(LIGHTS_RELAY_PIN, on ? HIGH : LOW);
  WifiOta::logf("[Lights] %s\n", on ? "ON" : "OFF");
  time_t ts; time(&ts);
  SdLogger::logEvent(ts, on ? "Lights ON" : "Lights OFF");
}

void setFilter(bool on) {
  if (filterOn == on) return;
  filterOn = on;
  // Normally-closed: LOW = relay off = contacts closed = filter ON
  //                  HIGH = relay on  = contacts open  = filter OFF
  digitalWrite(FILTER_RELAY_PIN, on ? LOW : HIGH);
  WifiOta::logf("[Filter] %s\n", on ? "ON" : "OFF");
  time_t ts; time(&ts);
  SdLogger::logEvent(ts, on ? "Filter ON" : "Filter OFF");
}

// ---------------------------------------------------------------------------
// Draw helpers
// ---------------------------------------------------------------------------

// Center text horizontally within a rectangle, baseline at y.
static void drawCenteredText(const char* s, int rectX, int rectW, int y) {
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(s, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(rectX + (rectW - (int)tw) / 2 - x1, y);
  tft.print(s);
}

// ---------------------------------------------------------------------------
// Splash screen — shown during setup() to hide the chaotic startup state.
// Call with increasing pct (0-100) and a status string; static elements are
// drawn only on the first call.
// ---------------------------------------------------------------------------
void drawSplash(int pct, const char* status) {
  static bool firstCall = true;
  if (firstCall) {
    firstCall = false;
    tft.fillScreen(C_BG);

    tft.setFont(&FreeSansBold24pt7b);
    tft.setTextColor(C_WHITE);
    drawCenteredText("BirksBeamer", 0, SCREEN_W, 105);

    tft.setFont(&FreeSansBold18pt7b);
    tft.setTextColor(C_AMBER);
    drawCenteredText("Aquarium", 0, SCREEN_W, 148);

    tft.drawRoundRect(40, 220, 400, 16, 7, C_DIVIDER);
  }

  int fillW = constrain((398 * pct) / 100, 0, 398);
  if (fillW > 0)
    tft.fillRect(41, 221, fillW, 14, C_BLUE);

  tft.fillRect(0, 248, SCREEN_W, 22, C_BG);
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(C_DIM);
  drawCenteredText(status, 0, SCREEN_W, 264);
}

// ---------------------------------------------------------------------------
// Water-change reminder — drawn in header center zone (x 110-285).
// Shows nothing when >3 days out or never logged. Amber within 3 days, red when overdue.
// Normal-mode fill starts at x=160 to avoid overwriting the "AQUARIUM" title (~x=12-155).
// Maint mode fills from x=110 to extend the amber header seamlessly.
// ---------------------------------------------------------------------------
void drawWaterChangeReminder() {
  bool maint = (maintState == MAINT_ACTIVE);
  if (maint) {
    tft.fillRect(110, 0, 175, HEADER_H, C_AMBER);
    return;
  }
  tft.fillRect(160, 0, 125, HEADER_H, C_HEADER);  // x=160-285, clears reminder zone only
  if (WebUi::lastWaterChange == 0) return;

  time_t now; time(&now);
  struct tm t;
  if (!getLocalTime(&t, 0) || t.tm_year + 1900 < 2024) return;

  int daysSince = (int)((now - (time_t)WebUi::lastWaterChange) / 86400);
  int daysLeft  = WebUi::waterChangeInterval - daysSince;
  if (daysLeft > 3) return;

  char buf[24];
  uint16_t col;
  if (daysLeft > 1) {
    snprintf(buf, sizeof(buf), "Change in %d days", daysLeft);
    col = C_AMBER;
  } else if (daysLeft == 1) {
    strcpy(buf, "Change tomorrow");
    col = C_AMBER;
  } else if (daysLeft == 0) {
    strcpy(buf, "Change due today");
    col = C_AMBER;
  } else {
    snprintf(buf, sizeof(buf), "Overdue %d day%s", -daysLeft, -daysLeft == 1 ? "" : "s");
    col = C_RED;
  }

  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(col);
  drawCenteredText(buf, 160, 125, 26);
}

// ---------------------------------------------------------------------------
// Draw: header bar — title + WiFi indicator only (no touch target here)
// ---------------------------------------------------------------------------
void drawHeader() {
  bool maint = (maintState == MAINT_ACTIVE);
  uint16_t headerBg = maint ? C_AMBER : C_HEADER;
  tft.fillRect(0, 0, SCREEN_W, HEADER_H, headerBg);

  // Paint zone fills before any text so nothing gets overwritten.
  updateClockDisplay();
  drawWaterChangeReminder();

  tft.setFont(&FreeSansBold12pt7b);
  tft.setTextColor(maint ? C_BG : C_WHITE);
  tft.setCursor(12, 28);
  tft.print(maint ? "WATER CHANGE" : "AQUARIUM");

  // WiFi status dot + label (moved right to clear "WATER CHANGE" title)
  tft.fillCircle(295, 20, 7, WifiOta::isConnected() ? C_GREEN : C_RED);

  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(WifiOta::isConnected() ? C_GREEN : C_RED);
  tft.setCursor(309, 25);
  tft.print(WifiOta::isConnected() ? "WiFi" : "No WiFi");
}

// ---------------------------------------------------------------------------
// Draw: temperature panel (left half of main area)
// ---------------------------------------------------------------------------

// Draw only the water-change button (called on state transitions to avoid
// full panel redraw when only the button label/color changes).
void drawWaterChangeButton() {
  const char* label;
  uint16_t bg;
  if (maintState == MAINT_CONFIRM) {
    label = "CONFIRM?"; bg = C_AMBER;
  } else if (maintState == MAINT_ACTIVE) {
    label = "RESUME"; bg = C_RED;
  } else if (maintState == MAINT_DONE_CONFIRM) {
    label = "LOG CHANGE?"; bg = C_GREEN;
  } else {
    label = "WATER CHANGE"; bg = C_BLUE_DIM;
  }
  uint16_t textCol;
  if      (maintState == MAINT_NORMAL)       textCol = C_DIM;
  else if (maintState == MAINT_DONE_CONFIRM) textCol = C_BG;
  else                                       textCol = C_WHITE;
  tft.fillRoundRect(WCHG_X, WCHG_Y, WCHG_W, WCHG_H, 10, bg);
  tft.setFont(&FreeSansBold12pt7b);
  tft.setTextColor(textCol);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(label, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(WCHG_X + (WCHG_W - (int)tw) / 2 - x1,
                WCHG_Y + (WCHG_H + (int)th) / 2);
  tft.print(label);
}

void drawTempPanel() {
  tft.fillRect(0, MAIN_Y, SPLIT_X, MAIN_H, C_PANEL);

  // -- Large temperature readout (or sensor error) --
  if (!tempValid) {
    tft.setFont(&FreeSansBold24pt7b);
    tft.setTextColor(C_RED);
    drawCenteredText("!", 0, SPLIT_X, MAIN_Y + 55);
    tft.setFont(&FreeSans12pt7b);
    tft.setTextColor(C_RED);
    drawCenteredText("Sensor error", 0, SPLIT_X, MAIN_Y + 90);
  } else {
    char tbuf[12];
    snprintf(tbuf, sizeof(tbuf), "%.1f\xB0""F", currentTemp);
    tft.setFont(&FreeSansBold24pt7b);
    tft.setTextColor(C_WHITE);
    drawCenteredText(tbuf, 0, SPLIT_X, MAIN_Y + 55);

    float delta = currentTemp - SET_POINT;
    const char* statusStr;
    uint16_t statusCol;
    if (currentTemp > SET_POINT + 3.0f) {
      statusStr = "HIGH TEMP"; statusCol = C_RED;
    } else if (currentTemp < SET_POINT - 3.0f) {
      statusStr = "LOW TEMP";  statusCol = C_RED;
    } else if (fabsf(delta) <= HEATER_HYST) {
      statusStr = "At target"; statusCol = C_GREEN;
    } else if (heaterOn) {
      statusStr = "Warming";   statusCol = C_AMBER;
    } else if (delta < 0) {
      statusStr = "Below target"; statusCol = C_AMBER;
    } else {
      statusStr = "Cooling";   statusCol = C_BLUE;
    }
    tft.setFont(&FreeSans12pt7b);
    tft.setTextColor(statusCol);
    drawCenteredText(statusStr, 0, SPLIT_X, MAIN_Y + 87);

    char tgtbuf[20];
    snprintf(tgtbuf, sizeof(tgtbuf), "Target: %.1f\xB0""F", SET_POINT);
    tft.setFont(&FreeSans9pt7b);
    tft.setTextColor(C_DIM);
    drawCenteredText(tgtbuf, 0, SPLIT_X, MAIN_Y + 108);
  }

  // -- Heater badge --
  updateHeaterBadge();

  // -- Sparkline --
  drawSparkline();
}

// ---------------------------------------------------------------------------
// Draw: clock in header (right-justified, matches AQUARIUM font)
// ---------------------------------------------------------------------------
void updateClockDisplay() {
  bool maint = (maintState == MAINT_ACTIVE);
  uint16_t headerBg  = maint ? C_AMBER : C_HEADER;
  uint16_t timeColor = maint ? C_BG    : C_WHITE;
  uint16_t ampmColor = maint ? C_BG    : C_GRAY;

  tft.fillRect(360, 0, SCREEN_W - 360, HEADER_H, headerBg);

  struct tm t;
  if (!getLocalTime(&t, 0) || t.tm_year + 1900 < 2024) {
    tft.setFont(&FreeSansBold12pt7b);
    tft.setTextColor(maint ? C_BG : C_DIM);
    int16_t x1, y1; uint16_t tw, th;
    tft.getTextBounds("--:--", 0, 0, &x1, &y1, &tw, &th);
    tft.setCursor(SCREEN_W - 8 - (int)tw - x1, 28);
    tft.print("--:--");
    return;
  }

  int hour12 = t.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;
  char tbuf[8];
  snprintf(tbuf, sizeof(tbuf), "%d:%02d", hour12, t.tm_min);

  // Measure AM/PM first so the whole group can be right-justified together
  const char* ampm = t.tm_hour < 12 ? "AM" : "PM";
  tft.setFont(&FreeSans9pt7b);
  int16_t ax1, ay1; uint16_t atw, ath;
  tft.getTextBounds(ampm, 0, 0, &ax1, &ay1, &atw, &ath);
  int ampmX = SCREEN_W - 8 - (int)atw - ax1;

  // Time sits 4px to the left of AM/PM
  tft.setFont(&FreeSansBold12pt7b);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(tbuf, 0, 0, &x1, &y1, &tw, &th);
  int timeX = ampmX - 4 - (int)tw - x1;
  tft.setTextColor(timeColor);
  tft.setCursor(timeX, 28);
  tft.print(tbuf);

  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(ampmColor);
  tft.setCursor(ampmX, 28);
  tft.print(ampm);
}

// ---------------------------------------------------------------------------
// Draw: lights panel (right half of main area)
// ---------------------------------------------------------------------------
void drawLightsPanel() {
  tft.fillRect(SPLIT_X, MAIN_Y, SCREEN_W - SPLIT_X, MAIN_H, C_BG);
  tft.drawFastVLine(SPLIT_X, MAIN_Y, MAIN_H, C_DIVIDER);

  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(C_GRAY);
  tft.setCursor(SPLIT_X + 15, MAIN_Y + 20);
  tft.print("LIGHTS");

  updateLightsStatus();
  drawWaterChangeButton();
}

// ---------------------------------------------------------------------------
// Draw: current-sensor row (bottom bar)
// Split into labels (static, drawn once) and values (updated every 2s).
// ---------------------------------------------------------------------------

// Redraws only the status value row. Skips redraw if nothing visible changed.
// Pass force=true after a full panel repaint (drawSensorRow) to ensure the
// cleared background is always repopulated.
void updateSensorValues(bool force = false) {
  bool sL = avgPkPkLights > THRESH_LIGHTS;
  bool sH = avgPkPkHeater > THRESH_HEATER;
  bool sF = avgPkPkFilter > THRESH_FILTER;

  static bool lastSL = false, lastSH = false, lastSF = false;
  static bool lastCL = false, lastCH = false, lastCF = false;
  static bool fresh  = true;

  if (!force && !fresh &&
      sL == lastSL && sH == lastSH && sF == lastSF &&
      lightsOn == lastCL && heaterOn == lastCH && filterOn == lastCF) return;

  fresh = false;
  lastSL = sL; lastSH = sH; lastSF = sF;
  lastCL = lightsOn; lastCH = heaterOn; lastCF = filterOn;

  tft.fillRect(0, SENSOR_Y + 17, SCREEN_W, 22, C_HEADER);
  tft.setFont(&FreeSans9pt7b);

  tft.setTextColor(sL == lightsOn ? C_DIM : C_RED);
  tft.setCursor(8, SENSOR_Y + 32);
  tft.print(sL ? "ON" : "OFF");

  tft.setTextColor(sH == heaterOn ? C_DIM : C_RED);
  tft.setCursor(168, SENSOR_Y + 32);
  tft.print(sH ? "ON" : "OFF");

  tft.setTextColor(sF == filterOn ? C_DIM : C_RED);
  tft.setCursor(330, SENSOR_Y + 32);
  tft.print(sF ? "ON" : "OFF");
}

// ---------------------------------------------------------------------------
// Sparkline — temperature history in the lower temp panel
// ---------------------------------------------------------------------------
void drawSparkline() {
  static const int SX = 0;
  static const int SY = MAIN_Y + 170;
  static const int SW = SPLIT_X;
  static const int SH = 67;

  tft.fillRect(SX, SY, SW, SH, C_PANEL);

  int count = tempHistCount;
  if (count < 2) return;

  int start = (count < 180) ? 0 : tempHistHead;

  // Fixed scale: thermostat operating band.  Values outside are clamped to edge.
  float mn    = SET_POINT - HEATER_HYST;
  float mx    = SET_POINT + HEATER_HYST;
  float range = mx - mn;

  // SET_POINT reference line
  if (SET_POINT >= mn && SET_POINT <= mx) {
    float norm = (SET_POINT - mn) / range;
    int refY = SY + SH - 1 - (int)(norm * (SH - 1));
    tft.drawFastHLine(SX, constrain(refY, SY, SY + SH - 1), SW, C_DIM);
  }

  // Polyline
  int prevX = -1, prevY = -1;
  for (int i = 0; i < count; i++) {
    float v = tempHistory[(start + i) % 180];
    int x = SX + (i * (SW - 1)) / (count - 1);
    int y = SY + SH - 1 - (int)((v - mn) / range * (SH - 1));
    y = constrain(y, SY, SY + SH - 1);
    if (prevX >= 0) tft.drawLine(prevX, prevY, x, y, C_BLUE);
    prevX = x; prevY = y;
  }
}

void drawSensorRow() {
  tft.fillRect(0, SENSOR_Y, SCREEN_W, SENSOR_H, C_HEADER);
  tft.drawFastHLine(0, SENSOR_Y, SCREEN_W, C_DIVIDER);
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(C_GRAY);
  tft.setCursor(8,   SENSOR_Y + 16); tft.print("LIGHTS");
  tft.setCursor(168, SENSOR_Y + 16); tft.print("HEATER");
  tft.setCursor(330, SENSOR_Y + 16); tft.print("FILTER");
  updateSensorValues(true);
}

// Full screen redraw
void drawStatusScreen() {
  tft.fillScreen(C_BG);
  drawHeader();
  drawTempPanel();
  drawLightsPanel();
  drawSensorRow();
}

// ---------------------------------------------------------------------------
// Targeted partial updates — clear only the bounding box of changing content
// rather than the whole panel, eliminating the blank-frame flicker.
// ---------------------------------------------------------------------------

// Redraws just the temperature number and delta line (not badge, not button).
// Clear zones are sized to the tallest/widest possible text for each element.
void updateTempValue() {
  tft.fillRect(0, MAIN_Y + 18, SPLIT_X, 44, C_PANEL);   // temp number row
  tft.fillRect(0, MAIN_Y + 71, SPLIT_X, 40, C_PANEL);   // status + target line rows

  if (!tempValid) {
    tft.setFont(&FreeSansBold24pt7b);
    tft.setTextColor(C_RED);
    drawCenteredText("!", 0, SPLIT_X, MAIN_Y + 55);
    tft.setFont(&FreeSans12pt7b);
    tft.setTextColor(C_RED);
    drawCenteredText("Sensor error", 0, SPLIT_X, MAIN_Y + 90);
    return;
  }

  char tbuf[12];
  snprintf(tbuf, sizeof(tbuf), "%.1f\xB0""F", currentTemp);
  tft.setFont(&FreeSansBold24pt7b);
  tft.setTextColor(C_WHITE);
  drawCenteredText(tbuf, 0, SPLIT_X, MAIN_Y + 55);

  float delta = currentTemp - SET_POINT;
  const char* statusStr;
  uint16_t statusCol;
  if (currentTemp > SET_POINT + 3.0f) {
    statusStr = "HIGH TEMP"; statusCol = C_RED;
  } else if (currentTemp < SET_POINT - 3.0f) {
    statusStr = "LOW TEMP";  statusCol = C_RED;
  } else if (fabsf(delta) <= HEATER_HYST) {
    statusStr = "At target"; statusCol = C_GREEN;
  } else if (heaterOn) {
    statusStr = "Warming";   statusCol = C_AMBER;
  } else if (delta < 0) {
    statusStr = "Below target"; statusCol = C_AMBER;
  } else {
    statusStr = "Cooling";   statusCol = C_BLUE;
  }
  tft.setFont(&FreeSans12pt7b);
  tft.setTextColor(statusCol);
  drawCenteredText(statusStr, 0, SPLIT_X, MAIN_Y + 87);

  char tgtbuf[20];
  snprintf(tgtbuf, sizeof(tgtbuf), "Target: %.1f\xB0""F", SET_POINT);
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(C_DIM);
  drawCenteredText(tgtbuf, 0, SPLIT_X, MAIN_Y + 108);
}

// Redraws the heater badge as a flat dot + label.
void updateHeaterBadge() {
  tft.fillRect(12, MAIN_Y + 120, SPLIT_X - 24, 44, C_PANEL);

  const char* label  = heaterOn ? "Heating" : "Idle";
  uint16_t    col    = heaterOn ? C_AMBER   : C_DIM;
  int         dotR   = heaterOn ? 6         : 3;
  const GFXfont* font = heaterOn ? &FreeSansBold12pt7b : &FreeSans12pt7b;

  tft.setFont(font);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(label, 0, 0, &x1, &y1, &tw, &th);

  // Center the dot+gap+text group horizontally in the badge area
  static const int gap = 8;
  int groupW = 2*dotR + gap + (int)tw;
  int badgeCX = 12 + (SPLIT_X - 24) / 2;   // 120
  int dotCX   = badgeCX - groupW / 2 + dotR;
  int dotCY   = MAIN_Y + 120 + 22;          // vertical centre of the 44px zone
  int textY   = MAIN_Y + 120 + (44 + (int)th) / 2;

  tft.fillCircle(dotCX, dotCY, dotR, col);
  tft.setTextColor(col);
  tft.setCursor(dotCX + dotR + gap - x1, textY);
  tft.print(label);
}

// Partial update: redraws only the "M:SS until auto-off" countdown line.
void updateCountdown() {
  unsigned long elapsed = millis() - lightsOverrideStart;
  if (elapsed > OVERRIDE_MS) elapsed = OVERRIDE_MS;
  int remaining = (int)((OVERRIDE_MS - elapsed) / 1000);
  char cbuf[24];
  snprintf(cbuf, sizeof(cbuf), "%d:%02d until auto-off", remaining / 60, remaining % 60);

  tft.fillRect(LPILL_X, LOVRD_CD_Y - 14, LPILL_W, 18, C_BG);
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(C_AMBER);
  drawCenteredText(cbuf, LPILL_X, LPILL_W, LOVRD_CD_Y);
}

// Redraws the full lights state zone. Three visual states:
//   override ON  — amber pill (tap to cancel) + countdown + next-schedule
//   scheduled ON — plain yellow ON text + next-schedule, no button
//   OFF          — OFF centered in a pill (tap to start override) + next-schedule
void updateLightsStatus() {
  // Clear the full zone between the "LIGHTS" label and the water-change button
  tft.fillRect(SPLIT_X + 1, MAIN_Y + 26, SCREEN_W - SPLIT_X - 1, WCHG_Y - MAIN_Y - 34, C_BG);

  char nxt[32];
  nextScheduleStr(nxt, sizeof(nxt));
  int16_t x1, y1; uint16_t tw, th;

  if (lightsOverride) {
    // Amber ON pill — tap anywhere on lights panel to cancel
    tft.fillRoundRect(LPILL_X, LOVRD_PILL_Y, LPILL_W, LOVRD_PILL_H, 8, C_AMBER);
    tft.setFont(&FreeSansBold18pt7b);
    tft.setTextColor(C_BG);
    tft.getTextBounds("ON", 0, 0, &x1, &y1, &tw, &th);
    tft.setCursor(LPILL_X + (LPILL_W - (int)tw) / 2 - x1,
                  LOVRD_PILL_Y + (LOVRD_PILL_H + (int)th) / 2);
    tft.print("ON");

    unsigned long elapsed = millis() - lightsOverrideStart;
    if (elapsed > OVERRIDE_MS) elapsed = OVERRIDE_MS;
    int remaining = (int)((OVERRIDE_MS - elapsed) / 1000);
    char cbuf[24];
    snprintf(cbuf, sizeof(cbuf), "%d:%02d until auto-off", remaining / 60, remaining % 60);
    tft.setFont(&FreeSans9pt7b);
    tft.setTextColor(C_AMBER);
    drawCenteredText(cbuf, LPILL_X, LPILL_W, LOVRD_CD_Y);

  } else if (lightsOn) {
    // Scheduled ON — plain text, no button
    tft.setFont(&FreeSansBold18pt7b);
    tft.setTextColor(C_YELLOW);
    tft.getTextBounds("ON", 0, 0, &x1, &y1, &tw, &th);
    static const int zoneH = WCHG_Y - MAIN_Y - 34;
    int zoneCY = (MAIN_Y + 26) + zoneH / 2;
    tft.setCursor(SPLIT_X + (SCREEN_W - SPLIT_X - (int)tw) / 2 - x1,
                  zoneCY + (int)th / 2);
    tft.print("ON");
    if (nxt[0] && maintState != MAINT_ACTIVE) {
      tft.setFont(&FreeSans9pt7b);
      tft.setTextColor(C_DIM);
      drawCenteredText(nxt, SPLIT_X, SCREEN_W - SPLIT_X, zoneCY + (int)th / 2 + 22);
    }

  } else {
    // OFF pill — the whole pill is the tap target
    static const int PILL_Y = MAIN_Y + 68;
    static const int PILL_H = 56;
    tft.fillRoundRect(LPILL_X, PILL_Y, LPILL_W, PILL_H, 10, C_HEADER);
    tft.setFont(&FreeSansBold18pt7b);
    tft.setTextColor(C_GRAY);
    tft.getTextBounds("OFF", 0, 0, &x1, &y1, &tw, &th);
    tft.setCursor(LPILL_X + (LPILL_W - (int)tw) / 2 - x1,
                  PILL_Y + (PILL_H + (int)th) / 2);
    tft.print("OFF");
    if (nxt[0]) {
      tft.setFont(&FreeSans9pt7b);
      tft.setTextColor(C_DIM);
      drawCenteredText(nxt, LPILL_X, LPILL_W, PILL_Y + PILL_H + 22);
    }
  }
}

// ---------------------------------------------------------------------------
// Current sensor — multi-sample peak-to-peak
// Samples rapidly over ~32ms (≈ 1.9 AC cycles at 60 Hz).
// Returns peak-to-peak ADC count swing; near 0 means no AC current.
// ---------------------------------------------------------------------------
static int readCurrentPkPk(int pin) {
  int lo = 4095, hi = 0;
  for (int i = 0; i < CURR_SAMPLES; i++) {
    int v = analogRead(pin);
    if (v < lo) lo = v;
    if (v > hi) hi = v;
    delayMicroseconds(CURR_DELAY_US);
  }
  return hi - lo;
}

// ---------------------------------------------------------------------------
// Sensor reads
// ---------------------------------------------------------------------------
void readTemperature() {
  if (millis() - lastTempRead < TEMP_INTERVAL) return;
  lastTempRead = millis();

  tempSensor.requestTemperatures();
  float t = tempSensor.getTempFByIndex(0);
  if (t > 32 && t < 120) {
    bool wasValid = tempValid;
    tempValid = true;
    // Accumulate 5 readings (5 × 2s = 10s) then push one averaged sample
    tempAccum += t;
    tempAccumN++;
    if (tempAccumN >= 5) {
      tempHistory[tempHistHead] = tempAccum / tempAccumN;
      tempHistHead = (tempHistHead + 1) % 180;
      if (tempHistCount < 180) tempHistCount++;
      tempAccum = 0.0f;
      tempAccumN = 0;
      if (!displayAsleep) drawSparkline();
    }
    WebUi::setTempStatus(t, true);
    if (fabsf(t - currentTemp) > 0.05f) {
      currentTemp = t;
      if (!displayAsleep) updateTempValue();
    }
    if (!wasValid && !displayAsleep) updateTempValue();  // recover from fault display
  } else {
    bool wasValid = tempValid;
    if (wasValid) WifiOta::logf("[Temp] SENSOR FAULT (%.1f) — heater forced OFF\n", t);
    tempValid = false;
    WebUi::setTempStatus(t, false);
    setHeater(false);
    if (wasValid && !displayAsleep) updateTempValue();
  }
}

void readCurrentSensors() {
  if (millis() - lastSensorRead < SENSOR_INTERVAL) return;
  lastSensorRead = millis();

  // Each channel blocks ~32ms; three channels = ~96ms total, every 2 seconds.
  pkpkLights = readCurrentPkPk(LIGHTS_SENSE_PIN);
  pkpkHeater = readCurrentPkPk(HEATER_SENSE_PIN);
  pkpkFilter = readCurrentPkPk(FILTER_SENSE_PIN);

  // Push raw readings into ring buffers and recompute windowed averages
  currHistLights[currHistHead] = pkpkLights;
  currHistHeater[currHistHead] = pkpkHeater;
  currHistFilter[currHistHead] = pkpkFilter;
  currHistHead = (currHistHead + 1) % CURR_HIST_MAX;
  if (currHistCount < CURR_HIST_MAX) currHistCount++;

  int win = constrain(WebUi::currAvgWindow, 1, currHistCount);
  long sumL = 0, sumH = 0, sumF = 0;
  for (int i = 0; i < win; i++) {
    int idx = (currHistHead - 1 - i + CURR_HIST_MAX) % CURR_HIST_MAX;
    sumL += currHistLights[idx];
    sumH += currHistHeater[idx];
    sumF += currHistFilter[idx];
  }
  avgPkPkLights = (int)(sumL / win);
  avgPkPkHeater = (int)(sumH / win);
  avgPkPkFilter = (int)(sumF / win);

  WifiOta::logf("[Sense] LT:%d(%d) HT:%d(%d) FT:%d(%d)\n",
               pkpkLights, (int)lightsOn,
               pkpkHeater, (int)heaterOn,
               pkpkFilter, (int)filterOn);

  time_t now;
  time(&now);
  SdLogger::logSample(now, currentTemp, heaterOn, lightsOn, filterOn,
                      pkpkLights, pkpkHeater, pkpkFilter);

  if (!displayAsleep) updateSensorValues();
}

// ---------------------------------------------------------------------------
// Control logic
// ---------------------------------------------------------------------------
void controlHeater() {
  if (!tempValid)           { setHeater(false); return; }
  if (maintState == MAINT_ACTIVE) { setHeater(false); return; }
  bool prev = heaterOn;
  if (heaterOn) {
    if (currentTemp >= SET_POINT + HEATER_HYST) setHeater(false);
  } else {
    if (currentTemp < SET_POINT - HEATER_HYST) setHeater(true);
  }
  if (prev != heaterOn && !displayAsleep) updateHeaterBadge();
}

void controlFilter() {
  if (maintState == MAINT_ACTIVE) { setFilter(false); return; }
  setFilter(true);
}

void controlLights() {
  if (maintState == MAINT_ACTIVE) return;

  // Rate-limit NTP schedule checks — getLocalTime with 0ms timeout is unreliable
  // when called every loop() iteration and can oscillate, toggling the relay.
  static bool schedOn = false;
  static bool scheduleChecked = false;
  static unsigned long lastScheduleCheck = 0;
  unsigned long now = millis();
  if (!scheduleChecked || now - lastScheduleCheck >= 30000UL) {
    schedOn = isScheduleTime(schedOn);  // pass current schedOn as fallback so
    lastScheduleCheck = now;            // an NTP hiccup doesn't turn lights off
    scheduleChecked = true;
  }

  bool want = schedOn || lightsOverride;
  static bool prevOverride = false;
  bool overrideChanged = (prevOverride != lightsOverride);
  prevOverride = lightsOverride;
  if (lightsOn != want) {
    setLights(want);
    if (!displayAsleep) updateLightsStatus();
  } else if (overrideChanged && !displayAsleep) {
    updateLightsStatus();
  }
}

// ---------------------------------------------------------------------------
// Lights override timer
// ---------------------------------------------------------------------------
void updateLightsOverride() {
  if (!lightsOverride) return;

  if (millis() - lightsOverrideStart >= OVERRIDE_MS) {
    lightsOverride = false;
    lastActivity = millis();  // keep display awake to show lights changing
    // controlLights() runs next and handles setLights + display update
    return;
  }

  if (millis() - lastCountdownDraw >= 1000) {
    lastCountdownDraw = millis();
    if (!displayAsleep) updateCountdown();
  }
}

// ---------------------------------------------------------------------------
// Water-change confirm timeout
// ---------------------------------------------------------------------------
void checkMaintConfirm() {
  if (maintState != MAINT_CONFIRM && maintState != MAINT_DONE_CONFIRM) return;
  if (millis() - maintConfirmAt >= CONFIRM_MS) {
    maintState = MAINT_NORMAL;
    if (!displayAsleep) drawWaterChangeButton();
  }
}

// ---------------------------------------------------------------------------
// Display sleep
// ---------------------------------------------------------------------------
void wakeDisplay() {
  displayAsleep = false;
  lastActivity = millis();
  digitalWrite(TFT_LITE_PIN, HIGH);
  drawStatusScreen();
}

void checkDisplaySleep() {
  if (maintState == MAINT_ACTIVE) { lastActivity = millis(); return; }
  if (!displayAsleep && millis() - lastActivity > SLEEP_MS) {
    displayAsleep = true;
    digitalWrite(TFT_LITE_PIN, LOW);
    WifiOta::logf("[Display] Sleep\n");
  }
}

// ---------------------------------------------------------------------------
// Clock — redraw when minute changes
// ---------------------------------------------------------------------------
void updateClock() {
  static int lastMin = -1;
  static unsigned long lastCheck = 0;
  unsigned long now = millis();
  if (now - lastCheck < 1000) return;
  lastCheck = now;

  struct tm t;
  if (!getLocalTime(&t, 0)) return;
  if (t.tm_min == lastMin) return;
  lastMin = t.tm_min;
  if (!displayAsleep) {
    updateClockDisplay();
    drawWaterChangeReminder();
  }
}

// ---------------------------------------------------------------------------
// WiFi status — redraw header dot on change
// ---------------------------------------------------------------------------
void checkWifiStatus() {
  static bool lastWifi = false;
  bool now = WifiOta::isConnected();
  if (now != lastWifi) {
    lastWifi = now;
    if (!displayAsleep) drawHeader();
  }
}

// ---------------------------------------------------------------------------
// Touch handling
// ---------------------------------------------------------------------------
static inline bool hit(int tx, int ty, int x, int y, int w, int h) {
  return tx >= x && tx < x + w && ty >= y && ty < y + h;
}

// Replaces library read_touch(): averages 3 X/Y pairs instead of requiring
// two consecutive readings within 100 counts. The library's strict consistency
// check rejects light touches whose contact point shifts slightly between the
// two rapid measurements. Averaging is more tolerant while keeping accuracy
// well within the ~10px we need for UI hit-testing.
static bool readTouch(uint16_t *x, uint16_t *y, uint16_t *z1, uint16_t *z2) {
  *z1 = touch.command(MEASURE_Z1, ADON_IRQOFF, ADC_12BIT);
  *z2 = touch.command(MEASURE_Z2, ADON_IRQOFF, ADC_12BIT);
  if (*z1 == 0) {
    touch.command(MEASURE_TEMP0, POWERDOWN_IRQON, ADC_12BIT);
    return false;
  }
  long sumX = 0, sumY = 0;
  for (int i = 0; i < 3; i++) {
    sumX += touch.command(MEASURE_X, ADON_IRQOFF, ADC_12BIT);
    sumY += touch.command(MEASURE_Y, ADON_IRQOFF, ADC_12BIT);
  }
  touch.command(MEASURE_TEMP0, POWERDOWN_IRQON, ADC_12BIT);
  *x = (uint16_t)(sumX / 3);
  *y = (uint16_t)(sumY / 3);
  return (*x != 4095) && (*y != 4095);
}

void handleTouch() {
  uint16_t rx, ry, z1, z2;
  if (!readTouch(&rx, &ry, &z1, &z2)) return;
  if (z1 < 30) return;

  // Coordinate mapping for setRotation(1).
  // Serial log lets you verify corner mapping without USB access.
  int sx = map(ry, 0, 4095, 0, SCREEN_W);
  int sy = map(rx, 4095, 0, 0, SCREEN_H);
  WifiOta::logf("[Touch] raw=(%u,%u) z1=%u  screen=(%d,%d)\n", rx, ry, z1, sx, sy);

  if (displayAsleep) {
    wakeDisplay();
    delay(200);
    return;
  }
  lastActivity = millis();

  // -- Water-change button (left panel, large hit target) --
  if (hit(sx, sy, WCHG_X, WCHG_Y, WCHG_W, WCHG_H)) {
    if (maintState == MAINT_NORMAL) {
      maintState = MAINT_CONFIRM;
      maintConfirmAt = millis();
      drawWaterChangeButton();

    } else if (maintState == MAINT_CONFIRM) {
      maintState = MAINT_ACTIVE;
      WifiOta::logf("[Maint] Entering water-change mode\n");
      setFilter(false);
      setHeater(false);
      setLights(true);
      drawStatusScreen();

    } else if (maintState == MAINT_ACTIVE) {
      if (currentTemp >= MAINT_EXIT_MIN_TEMP) {
        maintState = MAINT_DONE_CONFIRM;
        maintConfirmAt = millis();
        WifiOta::logf("[Maint] Exiting water-change mode\n");
        drawStatusScreen();  // header/relays restore; button shows LOG CHANGE?
      } else {
        // Flash warning in the button area, then restore
        tft.fillRoundRect(WCHG_X, WCHG_Y, WCHG_W, WCHG_H, 10, C_RED);
        tft.setFont(&FreeSans9pt7b);
        tft.setTextColor(C_WHITE);
        tft.setCursor(WCHG_X + 8, WCHG_Y + 18);
        tft.print("Can't resume yet:");
        char wb[28];
        snprintf(wb, sizeof(wb), "Temp %.1f\xB0""F < 74\xB0""F", currentTemp);
        tft.setCursor(WCHG_X + 8, WCHG_Y + 38);
        tft.print(wb);
        delay(2500);
        drawWaterChangeButton();
      }
    } else if (maintState == MAINT_DONE_CONFIRM) {
      WebUi::logWaterChange();
      maintState = MAINT_NORMAL;
      if (!displayAsleep) {
        drawWaterChangeButton();
        drawWaterChangeReminder();
      }
    }
    delay(200);
    return;
  }

  // -- Lights panel tap (right half of main area) --
  if (maintState != MAINT_ACTIVE &&
      hit(sx, sy, SPLIT_X, MAIN_Y, SCREEN_W - SPLIT_X, MAIN_H)) {
    if (lightsOverride) {
      lightsOverride = false;
      WifiOta::logf("[Lights] Override cancelled\n");
      updateLightsStatus();
    } else if (!lightsOn) {
      lightsOverride = true;
      lightsOverrideStart = millis();
      lastCountdownDraw = millis();
      setLights(true);
      updateLightsStatus();
    }
    delay(200);
    return;
  }

  delay(200);
}

// ---------------------------------------------------------------------------
// Setup & loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("== Aquarium Controller ==");

  // Belt-and-suspenders relay init.
  // GPIO 12 (HEATER) is a strap pin — must read LOW at boot.
  // Write before pinMode to prevent any transient HIGH glitch.
  digitalWrite(HEATER_RELAY_PIN, LOW);
  digitalWrite(LIGHTS_RELAY_PIN, LOW);
  digitalWrite(FILTER_RELAY_PIN, LOW);
  pinMode(HEATER_RELAY_PIN, OUTPUT);
  pinMode(LIGHTS_RELAY_PIN, OUTPUT);
  pinMode(FILTER_RELAY_PIN, OUTPUT);
  digitalWrite(HEATER_RELAY_PIN, LOW);
  digitalWrite(LIGHTS_RELAY_PIN, LOW);
  digitalWrite(FILTER_RELAY_PIN, LOW);  // NC relay: LOW = filter ON

  pinMode(LIGHTS_SENSE_PIN, INPUT);
  pinMode(HEATER_SENSE_PIN, INPUT);
  pinMode(FILTER_SENSE_PIN, INPUT);
  pinMode(TEMP_SENSOR_PIN, INPUT);

  pinMode(TFT_LITE_PIN, OUTPUT);
  digitalWrite(TFT_LITE_PIN, HIGH);

  tft.begin();
  tft.setRotation(1);
  drawSplash(0, "Starting up...");

  if (!touch.begin()) {
    tft.fillScreen(C_BG);
    tft.setFont(&FreeSans9pt7b);
    tft.setTextColor(C_RED);
    drawCenteredText("Touch controller not found", 0, SCREEN_W, 160);
    while (1) delay(100);
  }

  drawSplash(15, "Reading sensors...");
  tempSensor.begin();
  tempSensor.requestTemperatures();
  float t = tempSensor.getTempFByIndex(0);
  if (t > 32 && t < 120) currentTemp = t;

  // SD before WiFi — WiFi DMA init can disrupt SPI if SD runs after
  drawSplash(35, "Mounting SD card...");
  SdLogger::begin(SD_CS_PIN);

  drawSplash(55, "Connecting to WiFi...");
  WifiOta::begin();

  drawSplash(78, "Syncing time...");
  configTzTime(TZ_PACIFIC, "pool.ntp.org");

  drawSplash(92, "Starting web server...");
  WebUi::begin();

  drawSplash(100, "Ready!");
  delay(600);

  lastActivity = millis();
  drawStatusScreen();
}

void loop() {
  WifiOta::loop();
  WebUi::loop();
  readTemperature();
  readCurrentSensors();
  controlHeater();
  controlFilter();
  updateLightsOverride();
  controlLights();
  checkMaintConfirm();
  checkWifiStatus();
  updateClock();
  checkDisplaySleep();
  handleTouch();
}
