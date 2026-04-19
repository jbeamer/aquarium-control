#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_HX8357.h>
#include <Adafruit_TSC2007.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "wifi_ota.h"

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
#define TEMP_SENSOR_PIN   14

#define LIGHTS_RELAY_PIN  13   // active HIGH (also onboard red LED)
#define HEATER_RELAY_PIN  12   // active HIGH (GPIO 12 strap pin: must be LOW at boot)
#define FILTER_RELAY_PIN  27   // normally-closed: LOW=on, HIGH=off

#define LIGHTS_SENSE_PIN  34   // ADC1 ch6, input-only
#define HEATER_SENSE_PIN  39   // ADC1 ch3, input-only
#define FILTER_SENSE_PIN  36   // ADC1 ch0, input-only

// ---------------------------------------------------------------------------
// Colors — flat dark scheme
// ---------------------------------------------------------------------------
#define C_BG       0x0841   // very dark blue-gray
#define C_PANEL    0x18C3   // dark panel (left/temp side)
#define C_HEADER   0x1082   // header and sensor-row bar
#define C_DIVIDER  0x4A69   // panel border lines
#define C_WHITE    0xFFFF
#define C_GRAY     0x8410
#define C_DIM      0x39E7   // inactive / off state
#define C_GREEN    0x07E0
#define C_AMBER    0xFD20
#define C_RED      0xF800
#define C_YELLOW   0xFFE0

// ---------------------------------------------------------------------------
// Layout constants (all in pixels, absolute screen coords)
// Screen: 480 wide x 320 tall, setRotation(1)
// ---------------------------------------------------------------------------
#define SCREEN_W   480
#define SCREEN_H   320
#define HEADER_H    40
#define SENSOR_H    40
#define SPLIT_X    240          // x boundary between temp and lights panels
#define MAIN_Y     HEADER_H     // 40
#define MAIN_H     (SCREEN_H - HEADER_H - SENSOR_H)  // 240
#define SENSOR_Y   (SCREEN_H - SENSOR_H)             // 280

// Maintenance button in header
#define MAINT_BTN_X  283
#define MAINT_BTN_Y    5
#define MAINT_BTN_W  190
#define MAINT_BTN_H   30

// ---------------------------------------------------------------------------
// Thermostat config
// ---------------------------------------------------------------------------
#define SET_POINT           78.0f   // °F; will move to web config in phase 5
#define HEATER_HYST          0.3f   // heater on below (SET_POINT - HYST)
#define MAINT_EXIT_MIN_TEMP 74.0f   // minimum temp to allow exiting maintenance mode

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
#define TEMP_INTERVAL     2000UL
#define SENSOR_INTERVAL   2000UL
#define SLEEP_MS      (5UL * 60 * 1000)
#define OVERRIDE_MS   (5UL * 60 * 1000)
#define CONFIRM_MS    2000UL

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

// Maintenance / water-change mode
enum MaintState : uint8_t { MAINT_NORMAL, MAINT_CONFIRM, MAINT_ACTIVE };
MaintState maintState = MAINT_NORMAL;
unsigned long maintConfirmAt = 0;

// Display sleep
bool displayAsleep = false;
unsigned long lastActivity = 0;

// Current-sensor raw ADC readings
int senseLights = 0, senseHeater = 0, senseFilter = 0;
unsigned long lastSensorRead = 0;

// ---------------------------------------------------------------------------
// Relay helpers — idempotent, log on change
// ---------------------------------------------------------------------------
void setHeater(bool on) {
  if (heaterOn == on) return;
  heaterOn = on;
  digitalWrite(HEATER_RELAY_PIN, on ? HIGH : LOW);
  Serial.printf("[Heater] %s\n", on ? "ON" : "OFF");
}

void setLights(bool on) {
  if (lightsOn == on) return;
  lightsOn = on;
  digitalWrite(LIGHTS_RELAY_PIN, on ? HIGH : LOW);
  Serial.printf("[Lights] %s\n", on ? "ON" : "OFF");
}

void setFilter(bool on) {
  if (filterOn == on) return;
  filterOn = on;
  // Normally-closed relay: LOW = energized-off wait — actually:
  //   LOW  → relay not energized → NC contacts closed → filter ON
  //   HIGH → relay energized     → NC contacts open   → filter OFF
  digitalWrite(FILTER_RELAY_PIN, on ? LOW : HIGH);
  Serial.printf("[Filter] %s\n", on ? "ON" : "OFF");
}

// ---------------------------------------------------------------------------
// Draw: header bar
// ---------------------------------------------------------------------------
void drawHeader() {
  tft.fillRect(0, 0, SCREEN_W, HEADER_H, C_HEADER);

  tft.setFont(&FreeSansBold12pt7b);
  tft.setTextColor(C_WHITE);
  tft.setCursor(12, 28);
  tft.print("AQUARIUM");

  // WiFi status dot
  tft.fillCircle(252, 20, 7, WifiOta::isConnected() ? C_GREEN : C_RED);

  // Maintenance button — label and color reflect current state
  const char* label;
  uint16_t btnBg;
  if (maintState == MAINT_CONFIRM) {
    label = "CONFIRM?"; btnBg = C_AMBER;
  } else if (maintState == MAINT_ACTIVE) {
    label = "RESUME NORMAL"; btnBg = C_RED;
  } else {
    label = "MAINTENANCE"; btnBg = C_DIM;
  }

  tft.fillRoundRect(MAINT_BTN_X, MAINT_BTN_Y, MAINT_BTN_W, MAINT_BTN_H, 6, btnBg);
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(C_WHITE);
  int16_t bx, by; uint16_t bw, bh;
  tft.getTextBounds(label, 0, 0, &bx, &by, &bw, &bh);
  tft.setCursor(MAINT_BTN_X + (MAINT_BTN_W - (int)bw) / 2 - bx,
                MAINT_BTN_Y + (MAINT_BTN_H + (int)bh) / 2);
  tft.print(label);
}

// ---------------------------------------------------------------------------
// Draw: temperature panel (left half of main area)
// ---------------------------------------------------------------------------
void drawTempPanel() {
  tft.fillRect(0, MAIN_Y, SPLIT_X, MAIN_H, C_PANEL);

  int16_t x1, y1; uint16_t tw, th;

  // -- Large temperature readout --
  char tbuf[12];
  snprintf(tbuf, sizeof(tbuf), "%.1f\xB0""F", currentTemp);
  tft.setFont(&FreeSansBold24pt7b);
  tft.setTextColor(C_WHITE);
  tft.getTextBounds(tbuf, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor((SPLIT_X - (int)tw) / 2 - x1, MAIN_Y + 60);
  tft.print(tbuf);

  // -- Delta from target --
  float delta = currentTemp - SET_POINT;
  char dbuf[28];
  uint16_t dcol;
  if (fabsf(delta) <= 0.5f) {
    snprintf(dbuf, sizeof(dbuf), "At target");
    dcol = C_GREEN;
  } else if (delta < 0) {
    snprintf(dbuf, sizeof(dbuf), "%.1f\xB0 below target", -delta);
    dcol = C_AMBER;
  } else {
    snprintf(dbuf, sizeof(dbuf), "+%.1f\xB0 above target", delta);
    dcol = C_RED;
  }
  tft.setFont(&FreeSans12pt7b);
  tft.setTextColor(dcol);
  tft.getTextBounds(dbuf, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor((SPLIT_X - (int)tw) / 2 - x1, MAIN_Y + 93);
  tft.print(dbuf);

  // -- Heater on/off badge --
  const char* hlabel = heaterOn ? "HEATER  ON" : "HEATER  OFF";
  uint16_t hbg = heaterOn ? C_AMBER : C_DIM;
  uint16_t htx = heaterOn ? C_BG    : C_GRAY;
  tft.fillRoundRect(12, MAIN_Y + 104, SPLIT_X - 24, 44, 10, hbg);
  tft.setFont(&FreeSansBold12pt7b);
  tft.setTextColor(htx);
  tft.getTextBounds(hlabel, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(12 + (SPLIT_X - 24 - (int)tw) / 2 - x1,
                MAIN_Y + 104 + (44 + (int)th) / 2);
  tft.print(hlabel);

  // -- Maintenance overlay (lower portion, only when active) --
  if (maintState == MAINT_ACTIVE) {
    tft.fillRoundRect(12, MAIN_Y + 158, SPLIT_X - 24, 58, 8, C_RED);
    tft.setFont(&FreeSans9pt7b);
    tft.setTextColor(C_WHITE);
    tft.setCursor(22, MAIN_Y + 178);
    tft.print("MAINTENANCE MODE");
    tft.setCursor(22, MAIN_Y + 200);
    if (currentTemp < MAINT_EXIT_MIN_TEMP) {
      char wb[28];
      snprintf(wb, sizeof(wb), "Temp %.1f\xB0""F < 74\xB0""F", currentTemp);
      tft.print(wb);
    } else {
      tft.print("Tap RESUME NORMAL");
    }
  }
}

// ---------------------------------------------------------------------------
// Draw: lights panel (right half of main area)
// ---------------------------------------------------------------------------
void drawLightsPanel() {
  tft.fillRect(SPLIT_X, MAIN_Y, SCREEN_W - SPLIT_X, MAIN_H, C_BG);
  tft.drawFastVLine(SPLIT_X, MAIN_Y, MAIN_H, C_DIVIDER);

  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(C_GRAY);
  tft.setCursor(SPLIT_X + 15, MAIN_Y + 22);
  tft.print("LIGHTS");

  tft.setFont(&FreeSansBold18pt7b);
  tft.setTextColor(lightsOn ? C_YELLOW : C_DIM);
  tft.setCursor(SPLIT_X + 15, MAIN_Y + 75);
  tft.print(lightsOn ? "ON" : "OFF");

  if (!lightsOn) {
    tft.setFont(&FreeSans9pt7b);
    tft.setTextColor(C_GRAY);
    tft.setCursor(SPLIT_X + 15, MAIN_Y + 108);
    tft.print("Tap to enable 5 min");
  } else if (lightsOverride) {
    unsigned long elapsed = millis() - lightsOverrideStart;
    if (elapsed > OVERRIDE_MS) elapsed = OVERRIDE_MS;
    int remaining = (int)((OVERRIDE_MS - elapsed) / 1000);
    char cbuf[8];
    snprintf(cbuf, sizeof(cbuf), "%d:%02d", remaining / 60, remaining % 60);

    tft.setFont(&FreeSans12pt7b);
    tft.setTextColor(C_AMBER);
    tft.setCursor(SPLIT_X + 15, MAIN_Y + 112);
    tft.print(cbuf);

    tft.setFont(&FreeSans9pt7b);
    tft.setTextColor(C_GRAY);
    tft.setCursor(SPLIT_X + 15, MAIN_Y + 135);
    tft.print("remaining");
  }
}

// ---------------------------------------------------------------------------
// Draw: current-sensor row (bottom bar)
// ---------------------------------------------------------------------------
void drawSensorRow() {
  tft.fillRect(0, SENSOR_Y, SCREEN_W, SENSOR_H, C_HEADER);
  tft.drawFastHLine(0, SENSOR_Y, SCREEN_W, C_DIVIDER);

  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(C_GRAY);

  char buf[16];
  snprintf(buf, sizeof(buf), "LT: %4d", senseLights);
  tft.setCursor(8, SENSOR_Y + 28);
  tft.print(buf);

  snprintf(buf, sizeof(buf), "HT: %4d", senseHeater);
  tft.setCursor(168, SENSOR_Y + 28);
  tft.print(buf);

  snprintf(buf, sizeof(buf), "FT: %4d", senseFilter);
  tft.setCursor(330, SENSOR_Y + 28);
  tft.print(buf);
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
// Sensor reads
// ---------------------------------------------------------------------------
void readTemperature() {
  if (millis() - lastTempRead < TEMP_INTERVAL) return;
  lastTempRead = millis();

  tempSensor.requestTemperatures();
  float t = tempSensor.getTempFByIndex(0);
  if (t > 32 && t < 120 && fabsf(t - currentTemp) > 0.05f) {
    currentTemp = t;
    if (!displayAsleep) drawTempPanel();
  }
}

void readCurrentSensors() {
  if (millis() - lastSensorRead < SENSOR_INTERVAL) return;
  lastSensorRead = millis();

  senseLights = analogRead(LIGHTS_SENSE_PIN);
  senseHeater = analogRead(HEATER_SENSE_PIN);
  senseFilter = analogRead(FILTER_SENSE_PIN);
  if (!displayAsleep) drawSensorRow();
}

// ---------------------------------------------------------------------------
// Control logic
// ---------------------------------------------------------------------------
void controlHeater() {
  if (maintState == MAINT_ACTIVE) { setHeater(false); return; }
  setHeater(currentTemp < SET_POINT - HEATER_HYST);
}

void controlFilter() {
  if (maintState == MAINT_ACTIVE) { setFilter(false); return; }
  setFilter(true);
}

// ---------------------------------------------------------------------------
// Lights override timer
// ---------------------------------------------------------------------------
void updateLightsOverride() {
  if (!lightsOverride) return;

  if (millis() - lightsOverrideStart >= OVERRIDE_MS) {
    lightsOverride = false;
    setLights(false);
    if (!displayAsleep) drawLightsPanel();
    return;
  }

  // Redraw countdown once per second
  if (millis() - lastCountdownDraw >= 1000) {
    lastCountdownDraw = millis();
    if (!displayAsleep) drawLightsPanel();
  }
}

// ---------------------------------------------------------------------------
// Maintenance confirm timeout
// ---------------------------------------------------------------------------
void checkMaintConfirm() {
  if (maintState != MAINT_CONFIRM) return;
  if (millis() - maintConfirmAt >= CONFIRM_MS) {
    maintState = MAINT_NORMAL;
    if (!displayAsleep) drawHeader();
  }
}

// ---------------------------------------------------------------------------
// Display sleep
// ---------------------------------------------------------------------------
void wakeDisplay() {
  displayAsleep = false;
  lastActivity = millis();
  drawStatusScreen();
}

void checkDisplaySleep() {
  if (!displayAsleep && millis() - lastActivity > SLEEP_MS) {
    displayAsleep = true;
    tft.fillScreen(0x0000);
    Serial.println("[Display] Sleep");
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

void handleTouch() {
  uint16_t rx, ry, z1, z2;
  if (!touch.read_touch(&rx, &ry, &z1, &z2)) return;
  if (z1 < 100) return;

  // Coordinate mapping for setRotation(1).
  // Print raw values to serial to verify corner mapping on hardware.
  int sx = map(ry, 0, 4095, 0, SCREEN_W);
  int sy = map(rx, 4095, 0, 0, SCREEN_H);
  Serial.printf("[Touch] raw=(%u,%u) z1=%u  screen=(%d,%d)\n", rx, ry, z1, sx, sy);

  if (displayAsleep) {
    wakeDisplay();
    delay(200);
    return;
  }
  lastActivity = millis();

  // -- Maintenance button (header) --
  if (hit(sx, sy, MAINT_BTN_X, MAINT_BTN_Y, MAINT_BTN_W, MAINT_BTN_H)) {
    if (maintState == MAINT_NORMAL) {
      maintState = MAINT_CONFIRM;
      maintConfirmAt = millis();
      drawHeader();

    } else if (maintState == MAINT_CONFIRM) {
      maintState = MAINT_ACTIVE;
      Serial.println("[Maint] Entering maintenance mode");
      setFilter(false);
      setHeater(false);
      setLights(true);
      drawStatusScreen();

    } else if (maintState == MAINT_ACTIVE) {
      if (currentTemp >= MAINT_EXIT_MIN_TEMP) {
        maintState = MAINT_NORMAL;
        Serial.println("[Maint] Exiting maintenance mode");
        drawStatusScreen();
      } else {
        // Temp too low — flash warning in the overlay area, then restore
        tft.fillRoundRect(12, MAIN_Y + 158, SPLIT_X - 24, 58, 8, C_RED);
        tft.setFont(&FreeSans9pt7b);
        tft.setTextColor(C_WHITE);
        tft.setCursor(22, MAIN_Y + 178);
        tft.print("Can't resume yet:");
        tft.setCursor(22, MAIN_Y + 200);
        char wb[28];
        snprintf(wb, sizeof(wb), "Temp %.1f\xB0""F < 74\xB0""F", currentTemp);
        tft.print(wb);
        delay(2500);
        drawTempPanel();
      }
    }
    delay(200);
    return;
  }

  // -- Lights panel tap (right half of main area) --
  if (maintState != MAINT_ACTIVE &&
      hit(sx, sy, SPLIT_X, MAIN_Y, SCREEN_W - SPLIT_X, MAIN_H)) {
    if (!lightsOn) {
      lightsOverride = true;
      lightsOverrideStart = millis();
      lastCountdownDraw = millis();
      setLights(true);
      drawLightsPanel();
    }
    // If lights are already on, tap does nothing
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
  // Write before pinMode to avoid any transient HIGH glitch.
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

  tft.begin();
  tft.setRotation(1);

  if (!touch.begin()) {
    Serial.println("TSC2007 not found — halting");
    while (1) delay(100);
  }

  tempSensor.begin();
  tempSensor.requestTemperatures();
  float t = tempSensor.getTempFByIndex(0);
  if (t > 32 && t < 120) currentTemp = t;

  lastActivity = millis();
  drawStatusScreen();

  // WiFi + OTA last — display and sensors are already up if connection takes time
  WifiOta::begin();
}

void loop() {
  WifiOta::loop();
  readTemperature();
  readCurrentSensors();
  controlHeater();
  controlFilter();
  updateLightsOverride();
  checkMaintConfirm();
  checkWifiStatus();
  checkDisplaySleep();
  handleTouch();
}
