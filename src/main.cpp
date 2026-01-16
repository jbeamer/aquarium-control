#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_HX8357.h>
#include <Adafruit_TSC2007.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// Pin definitions
#define TFT_CS   15
#define TFT_DC   33
#define TEMP_SENSOR_PIN  14  // DS18B20 data pin

// Display and touch
Adafruit_HX8357 tft = Adafruit_HX8357(TFT_CS, TFT_DC);
Adafruit_TSC2007 touch;

// Temperature sensor
OneWire oneWire(TEMP_SENSOR_PIN);
DallasTemperature tempSensor(&oneWire);

// State variables
float currentTemp = 0.0;
float setPoint = 78.0;  // Default 78°F
unsigned long lastTempRead = 0;
const unsigned long TEMP_READ_INTERVAL = 2000;  // Read every 2 seconds

// Button definitions (in screen coordinates, landscape 480x320)
struct Button {
  int x, y, w, h;
  const char* label;
  uint16_t color;
};

Button btnTempUp = {350, 50, 100, 80, "+", HX8357_GREEN};
Button btnTempDown = {350, 180, 100, 80, "-", HX8357_RED};

void drawButton(Button &btn, bool pressed = false) {
  uint16_t color = pressed ? HX8357_WHITE : btn.color;
  uint16_t textColor = pressed ? btn.color : HX8357_WHITE;
  
  tft.fillRoundRect(btn.x, btn.y, btn.w, btn.h, 10, color);
  tft.drawRoundRect(btn.x, btn.y, btn.w, btn.h, 10, HX8357_WHITE);
  
  tft.setTextSize(5);
  tft.setTextColor(textColor);
  
  // Center the text
  int16_t x1, y1;
  uint16_t tw, th;
  tft.getTextBounds(btn.label, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(btn.x + (btn.w - tw) / 2, btn.y + (btn.h - th) / 2);
  tft.print(btn.label);
}

void drawThermostatUI() {
  tft.fillScreen(HX8357_BLACK);
  
  // Title
  tft.setTextSize(2);
  tft.setTextColor(HX8357_CYAN);
  tft.setCursor(10, 10);
  tft.println("AQUARIUM THERMOSTAT");
  
  // Current temperature - large display
  tft.setTextSize(6);
  tft.setTextColor(HX8357_WHITE);
  tft.setCursor(20, 80);
  tft.printf("%.1fF", currentTemp);
  
  // Set point
  tft.setTextSize(3);
  tft.setTextColor(HX8357_YELLOW);
  tft.setCursor(20, 180);
  tft.printf("Set: %.1fF", setPoint);
  
  // Status indicator
  tft.setTextSize(2);
  if (currentTemp < setPoint - 0.5) {
    tft.setTextColor(HX8357_RED);
    tft.setCursor(20, 240);
    tft.println("HEATING");
  } else if (currentTemp > setPoint + 0.5) {
    tft.setTextColor(HX8357_BLUE);
    tft.setCursor(20, 240);
    tft.println("IDLE");
  } else {
    tft.setTextColor(HX8357_GREEN);
    tft.setCursor(20, 240);
    tft.println("AT TEMP");
  }
  
  // Draw buttons
  drawButton(btnTempUp);
  drawButton(btnTempDown);
}

bool isButtonPressed(Button &btn, int touchX, int touchY) {
  return (touchX >= btn.x && touchX <= (btn.x + btn.w) &&
          touchY >= btn.y && touchY <= (btn.y + btn.h));
}

void handleTouch()
 {
  uint16_t x, y, z1, z2;
  
  // Try to read touch data (needs 4 parameters for TSC2007)
  if (!touch.read_touch(&x, &y, &z1, &z2)) {
    return;  // No touch detected
  }
  
  // Check if pressure is sufficient (z1 is the primary pressure reading)
  if (z1 < 100) {
    return;  // Not enough pressure
  }
  
  // Map touch coordinates to screen coordinates
  // Try direct mapping first
  int screenX = map(x, 0, 4095, 0, 480);
  int screenY = map(y, 0, 4095, 0, 320);
  
  Serial.printf("Touch: raw(%d,%d,z1:%d,z2:%d) screen(%d,%d)\n", x, y, z1, z2, screenX, screenY);
  
  // Check button presses
  if (isButtonPressed(btnTempUp, screenX, screenY)) {
    setPoint += 0.5;
    drawButton(btnTempUp, true);  // Visual feedback
    delay(100);
    drawThermostatUI();
    Serial.printf("Set point increased to %.1f\n", setPoint);
  } else if (isButtonPressed(btnTempDown, screenX, screenY)) {
    setPoint -= 0.5;
    drawButton(btnTempDown, true);  // Visual feedback
    delay(100);
    drawThermostatUI();
    Serial.printf("Set point decreased to %.1f\n", setPoint);
  }
  
  // Debounce
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
  
  // Sanity check
  if (newTemp > 32 && newTemp < 120) {
    if (abs(newTemp - currentTemp) > 0.1) {  // Only update if changed
      currentTemp = newTemp;
      drawThermostatUI();
      Serial.printf("Temperature: %.1fF\n", currentTemp);
    }
  } else {
    Serial.printf("Invalid temperature reading: %.1f\n", newTemp);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Aquarium Thermostat ===");
  
  // Initialize display
  tft.begin();
  tft.setRotation(3);  // Landscape, upside-down
  Serial.println("Display initialized");
  
  // Initialize touch controller
  if (!touch.begin()) {
    Serial.println("TSC2007 touch controller not found!");
    tft.fillScreen(HX8357_RED);
    tft.setCursor(10, 10);
    tft.setTextColor(HX8357_WHITE);
    tft.setTextSize(2);
    tft.println("Touch init failed!");
    while(1) delay(100);
  }
  Serial.println("Touch controller initialized");
  
  // Initialize temperature sensor
  tempSensor.begin();
  int deviceCount = tempSensor.getDeviceCount();
  Serial.printf("Found %d temperature sensor(s)\n", deviceCount);
  if (deviceCount == 0) {
    Serial.println("ERROR: No DS18B20 sensors found!");
    Serial.println("Check wiring and pullup resistor (4.7k ohm)");
  }

  Serial.println("Temperature sensor initialized");
  
  // Initial temperature read
  tempSensor.requestTemperatures();
  currentTemp = tempSensor.getTempFByIndex(0);
  Serial.printf("Initial temp reading: %.1fF\n", currentTemp);
  
  // Draw initial UI
  drawThermostatUI();
}

void loop() {
  readTemperature();
  handleTouch();
  delay(50);
}