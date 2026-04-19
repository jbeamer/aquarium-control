#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_HX8357.h>
#include <Adafruit_TSC2007.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#include "wifi_ota.h"

// Include better fonts
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans24pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

// ---------------------------------------------------------------------------
// Pin definitions
//
// TFT FeatherWing (fixed, do not change):
//   TFT_CS = 15, TFT_DC = 33, SPI = MOSI/MISO/SCK, TSC2007 touch over I2C.
//
// Relays: active-HIGH, wired left-to-right in the power box as Lights / Heater / Filter.
// Current sensors: all on ADC1 so they remain readable after WiFi starts.
// ---------------------------------------------------------------------------
#define TFT_CS            15
#define TFT_DC            33
#define TEMP_SENSOR_PIN   14

#define LIGHTS_RELAY_PIN  13   // active HIGH, normally open (also onboard red LED)
#define HEATER_RELAY_PIN  12   // active HIGH, normally open (STRAP PIN - must be LOW at boot)
#define FILTER_RELAY_PIN  27   // active HIGH, normally closed

#define LIGHTS_SENSE_PIN  34   // A2 - ADC1, input-only
#define HEATER_SENSE_PIN  39   // A3 - ADC1, input-only
#define FILTER_SENSE_PIN  36   // A4 - ADC1, input-only

// Aquarium-inspired color palette
#define COLOR_DEEP_OCEAN    0x0451
#define COLOR_WATER_BLUE    0x1E9F
#define COLOR_LIGHT_AQUA    0x5F3F
#define COLOR_CORAL_PINK    0xFB6C
#define COLOR_WARM_AMBER    0xFD20
#define COLOR_COOL_WHITE    0xFFFF
#define COLOR_SOFT_GRAY     0x8410
#define COLOR_SEAFOAM       0x87F3
#define COLOR_REEF_PURPLE   0x8017
#define COLOR_WARNING_RED   0xF800

// Display and touch
Adafruit_HX8357 tft = Adafruit_HX8357(TFT_CS, TFT_DC);
Adafruit_TSC2007 touch;

// Temperature sensor
OneWire oneWire(TEMP_SENSOR_PIN);
DallasTemperature tempSensor(&oneWire);

// State variables
float currentTemp = 74.0;
float setPoint = 78.0;
unsigned long lastTempRead = 0;
const unsigned long TEMP_READ_INTERVAL = 2000;

// Relay states
bool heaterOn = false;
bool lightsOn = false;
bool filterOn = true;
bool lightsManualOverride = false;

// Lights schedule
int lightsOnHour = 8;
int lightsOffHour = 20;

// Screen management
enum Screen { SCREEN_THERMOSTAT, SCREEN_LIGHTS };
Screen currentScreen = SCREEN_THERMOSTAT;

// Button definitions
struct Button {
  int x, y, w, h;
  const char* label;
  uint16_t color;
  uint16_t bgColor;
};

Button btnTempUp = {360, 80, 100, 70, "+", COLOR_COOL_WHITE, COLOR_SEAFOAM};
Button btnTempDown = {360, 170, 100, 70, "-", COLOR_COOL_WHITE, COLOR_CORAL_PINK};
Button btnToLights = {20, 250, 200, 35, "LIGHTS", COLOR_COOL_WHITE, COLOR_WATER_BLUE};

Button btnLightsToggle = {140, 100, 200, 80, "", COLOR_COOL_WHITE, COLOR_WARM_AMBER};
Button btnOnTimeUp = {360, 70, 90, 55, "+", COLOR_COOL_WHITE, COLOR_LIGHT_AQUA};
Button btnOnTimeDown = {360, 135, 90, 55, "-", COLOR_COOL_WHITE, COLOR_LIGHT_AQUA};
Button btnOffTimeUp = {360, 210, 90, 55, "+", COLOR_COOL_WHITE, COLOR_REEF_PURPLE};
Button btnOffTimeDown = {360, 275, 90, 55, "-", COLOR_COOL_WHITE, COLOR_REEF_PURPLE};
Button btnToThermostat = {20, 250, 200, 35, "TEMP", COLOR_COOL_WHITE, COLOR_WATER_BLUE};

void controlHeater() {
  bool shouldHeat = (currentTemp < setPoint - 0.3);
  
  if (shouldHeat != heaterOn) {
    heaterOn = shouldHeat;
    digitalWrite(HEATER_RELAY_PIN, heaterOn);
    Serial.printf("Heater turned %s\n", heaterOn ? "ON" : "OFF");
  }
}

void updateLights() {
  if (lightsManualOverride) {
    return;
  }
  // Schedule logic would go here with RTC
}

void drawButton(Button &btn, bool pressed = false) {
  uint16_t fillColor = pressed ? btn.color : btn.bgColor;
  uint16_t textColor = pressed ? btn.bgColor : btn.color;
  
  // Draw rounded rectangle button
  tft.fillRoundRect(btn.x, btn.y, btn.w, btn.h, 12, fillColor);
  tft.drawRoundRect(btn.x, btn.y, btn.w, btn.h, 12, btn.color);
  
  // Draw text centered
  tft.setFont(&FreeSansBold18pt7b);
  tft.setTextColor(textColor);
  
  int16_t x1, y1;
  uint16_t tw, th;
  tft.getTextBounds(btn.label, 0, 0, &x1, &y1, &tw, &th);
  
  // FreeSans fonts need Y adjustment for vertical centering
  int textX = btn.x + (btn.w - tw) / 2;
  int textY = btn.y + (btn.h + th) / 2 - 3;
  
  tft.setCursor(textX, textY);
  tft.print(btn.label);
}

void drawThermostatScreen() {
  // Gradient-like background (simulated with horizontal bands)
  for (int y = 0; y < 320; y += 4) {
    uint16_t color = 0x0020 + (y / 20);  // Subtle gradient
    tft.drawFastHLine(0, y, 480, color);
    tft.drawFastHLine(0, y+1, 480, color);
    tft.drawFastHLine(0, y+2, 480, color);
    tft.drawFastHLine(0, y+3, 480, color);
  }
  
  // Title bar
  tft.fillRect(0, 0, 480, 50, COLOR_WATER_BLUE);
  tft.setFont(&FreeSansBold12pt7b);
  tft.setTextColor(COLOR_COOL_WHITE);
  tft.setCursor(20, 32);
  tft.print("AQUARIUM CONTROLLER");
  
  // Temperature display - large and beautiful
  tft.setFont(&FreeSansBold24pt7b);
  tft.setTextColor(COLOR_COOL_WHITE);
  
  char tempStr[10];
  sprintf(tempStr, "%.1f°", currentTemp);
  
  int16_t x1, y1;
  uint16_t tw, th;
  tft.getTextBounds(tempStr, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(40, 130);
  tft.print(tempStr);
  
  // "F" in smaller font
  tft.setFont(&FreeSans18pt7b);
  tft.setCursor(40 + tw + 5, 130);
  tft.print("F");
  
  // Set point
  tft.setFont(&FreeSansBold12pt7b);
  tft.setTextColor(COLOR_LIGHT_AQUA);
  tft.setCursor(40, 175);
  tft.printf("Target: %.1f°F", setPoint);
  
  // Status indicator with icon-like display
  tft.fillRoundRect(30, 190, 300, 45, 10, COLOR_DEEP_OCEAN);
  tft.setFont(&FreeSans12pt7b);
  
  if (heaterOn) {
    tft.setTextColor(COLOR_WARM_AMBER);
    tft.setCursor(45, 220);
    tft.print("HEATING");
  } else if (currentTemp > setPoint + 0.5) {
    tft.setTextColor(COLOR_WATER_BLUE);
    tft.setCursor(45, 220);
    tft.print("STANDBY");
  } else {
    tft.setTextColor(COLOR_SEAFOAM);
    tft.setCursor(45, 220);
    tft.print("AT TARGET");
  }
  
  // Lights mini-status
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(lightsOn ? COLOR_WARM_AMBER : COLOR_SOFT_GRAY);
  tft.setCursor(250, 270);
  tft.printf("Lights: %s", lightsOn ? "ON" : "OFF");
  
  // Draw buttons
  drawButton(btnTempUp);
  drawButton(btnTempDown);
  drawButton(btnToLights);
}

void drawLightsScreen() {
  // Similar gradient background
  for (int y = 0; y < 320; y += 4) {
    uint16_t color = 0x0020 + (y / 20);
    tft.drawFastHLine(0, y, 480, color);
    tft.drawFastHLine(0, y+1, 480, color);
    tft.drawFastHLine(0, y+2, 480, color);
    tft.drawFastHLine(0, y+3, 480, color);
  }
  
  // Title bar
  tft.fillRect(0, 0, 480, 50, COLOR_WATER_BLUE);
  tft.setFont(&FreeSansBold12pt7b);
  tft.setTextColor(COLOR_COOL_WHITE);
  tft.setCursor(20, 32);
  tft.print("LIGHTING CONTROL");
  
  // Status display
  tft.fillRoundRect(30, 60, 280, 60, 15, lightsOn ? COLOR_WARM_AMBER : COLOR_DEEP_OCEAN);
  tft.setFont(&FreeSansBold18pt7b);
  tft.setTextColor(lightsOn ? COLOR_DEEP_OCEAN : COLOR_SOFT_GRAY);
  tft.setCursor(95, 100);
  tft.print(lightsOn ? "LIGHTS ON" : "LIGHTS OFF");
  
  // Mode indicator
  tft.setFont(&FreeSans9pt7b);
  tft.setTextColor(COLOR_LIGHT_AQUA);
  tft.setCursor(30, 140);
  tft.printf("Mode: %s", lightsManualOverride ? "MANUAL" : "AUTO");
  
  // Schedule section
  tft.fillRoundRect(20, 155, 330, 115, 10, COLOR_DEEP_OCEAN);
  
  tft.setFont(&FreeSansBold12pt7b);
  tft.setTextColor(COLOR_LIGHT_AQUA);
  tft.setCursor(35, 185);
  tft.print("ON TIME");
  
  tft.setFont(&FreeSans18pt7b);
  tft.setTextColor(COLOR_COOL_WHITE);
  tft.setCursor(190, 185);
  tft.printf("%02d:00", lightsOnHour);
  
  tft.setFont(&FreeSansBold12pt7b);
  tft.setTextColor(COLOR_REEF_PURPLE);
  tft.setCursor(35, 245);
  tft.print("OFF TIME");
  
  tft.setFont(&FreeSans18pt7b);
  tft.setTextColor(COLOR_COOL_WHITE);
  tft.setCursor(190, 245);
  tft.printf("%02d:00", lightsOffHour);
  
  // Update toggle button label
  btnLightsToggle.label = lightsOn ? "TURN OFF" : "TURN ON";
  btnLightsToggle.bgColor = lightsOn ? COLOR_CORAL_PINK : COLOR_SEAFOAM;
  
  // Draw buttons
  drawButton(btnLightsToggle);
  drawButton(btnOnTimeUp);
  drawButton(btnOnTimeDown);
  drawButton(btnOffTimeUp);
  drawButton(btnOffTimeDown);
  drawButton(btnToThermostat);
}

bool isButtonPressed(Button &btn, int touchX, int touchY) {
  return (touchX >= btn.x && touchX <= (btn.x + btn.w) &&
          touchY >= btn.y && touchY <= (btn.y + btn.h));
}

void handleThermostatTouch(int screenX, int screenY) {
  if (isButtonPressed(btnTempUp, screenX, screenY)) {
    setPoint += 0.5;
    drawButton(btnTempUp, true);
    delay(100);
    drawThermostatScreen();
  } else if (isButtonPressed(btnTempDown, screenX, screenY)) {
    setPoint -= 0.5;
    drawButton(btnTempDown, true);
    delay(100);
    drawThermostatScreen();
  } else if (isButtonPressed(btnToLights, screenX, screenY)) {
    currentScreen = SCREEN_LIGHTS;
    drawLightsScreen();
  }
}

void handleLightsTouch(int screenX, int screenY) {
  if (isButtonPressed(btnLightsToggle, screenX, screenY)) {
    lightsOn = !lightsOn;
    lightsManualOverride = true;
    digitalWrite(LIGHTS_RELAY_PIN, lightsOn);
    drawButton(btnLightsToggle, true);
    delay(100);
    drawLightsScreen();
  } else if (isButtonPressed(btnOnTimeUp, screenX, screenY)) {
    lightsOnHour = (lightsOnHour + 1) % 24;
    drawLightsScreen();
  } else if (isButtonPressed(btnOnTimeDown, screenX, screenY)) {
    lightsOnHour = (lightsOnHour - 1 + 24) % 24;
    drawLightsScreen();
  } else if (isButtonPressed(btnOffTimeUp, screenX, screenY)) {
    lightsOffHour = (lightsOffHour + 1) % 24;
    drawLightsScreen();
  } else if (isButtonPressed(btnOffTimeDown, screenX, screenY)) {
    lightsOffHour = (lightsOffHour - 1 + 24) % 24;
    drawLightsScreen();
  } else if (isButtonPressed(btnToThermostat, screenX, screenY)) {
    currentScreen = SCREEN_THERMOSTAT;
    drawThermostatScreen();
  }
}

void handleTouch() {
  uint16_t x, y, z1, z2;
  
  if (!touch.read_touch(&x, &y, &z1, &z2)) {
    return;
  }
  
  if (z1 < 100) {
    return;
  }
  
  int screenX = map(y, 4095, 0, 0, 480);
  int screenY = map(x, 0, 4095, 0, 320);
  
  if (currentScreen == SCREEN_THERMOSTAT) {
    handleThermostatTouch(screenX, screenY);
  } else if (currentScreen == SCREEN_LIGHTS) {
    handleLightsTouch(screenX, screenY);
  }
  
  delay(200);
}

void readTemperature() {
  unsigned long now = millis();
  if (now - lastTempRead < TEMP_READ_INTERVAL) {
    return;
  }
  
  lastTempRead = now;
  tempSensor.requestTemperatures();
  float newTemp = tempSensor.getTempFByIndex(0);
  
  if (newTemp > 32 && newTemp < 120) {
    if (abs(newTemp - currentTemp) > 0.1) {
      currentTemp = newTemp;
      if (currentScreen == SCREEN_THERMOSTAT) {
        drawThermostatScreen();
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("== Aquarium Controller ==");

  // --- Relay pin init ---
  // Belt-and-suspenders: drive all relay pins LOW via the GPIO matrix BEFORE
  // configuring them as outputs. This matters especially for HEATER_RELAY_PIN
  // (GPIO 12) which is a bootstrap strap pin - must read LOW at reset or the
  // ESP32 won't boot. Writing first, then setting pinMode, avoids any
  // transient HIGH glitch.
  digitalWrite(HEATER_RELAY_PIN, LOW);
  digitalWrite(LIGHTS_RELAY_PIN, LOW);
  digitalWrite(FILTER_RELAY_PIN, LOW);

  pinMode(HEATER_RELAY_PIN, OUTPUT);
  pinMode(LIGHTS_RELAY_PIN, OUTPUT);
  pinMode(FILTER_RELAY_PIN, OUTPUT);

  // Re-assert safe state after pinMode.
  digitalWrite(HEATER_RELAY_PIN, LOW);
  digitalWrite(LIGHTS_RELAY_PIN, LOW);
  digitalWrite(FILTER_RELAY_PIN, LOW);  // Normally-closed relay: LOW keeps filter ON

  // Current-sense pins (input-only on ADC1, no pinMode strictly required but
  // explicit is better than implicit).
  pinMode(LIGHTS_SENSE_PIN, INPUT);
  pinMode(HEATER_SENSE_PIN, INPUT);
  pinMode(FILTER_SENSE_PIN, INPUT);

  pinMode(TEMP_SENSOR_PIN, INPUT);

  // --- Display + touch ---
  tft.begin();
  tft.setRotation(3);

  if (!touch.begin()) {
    Serial.println("TSC2007 not found!");
    while(1) delay(100);
  }

  // --- Temp sensor ---
  tempSensor.begin();
  tempSensor.requestTemperatures();
  currentTemp = tempSensor.getTempFByIndex(0);

  drawThermostatScreen();

  // --- WiFi + OTA (non-blocking) ---
  // Kicked off last so the display and sensors are already up if the WiFi
  // connection takes a few seconds. OTA becomes available once connected.
  WifiOta::begin();
}

void loop() {
  WifiOta::loop();       // service OTA + reconnect logic
  readTemperature();
  controlHeater();
  updateLights();
  handleTouch();
  delay(50);
}
