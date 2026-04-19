// wifi_ota.cpp

#include "wifi_ota.h"
#include "config.h"

#include <WiFi.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>

namespace WifiOta {

namespace {

constexpr unsigned long RECONNECT_INTERVAL_MS = 10000;  // try every 10s if dropped
unsigned long lastReconnectAttempt = 0;
bool otaInitialized = false;

void initializeOta() {
  if (otaInitialized) return;

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  // No password - relying on local network trust per current config.
  // To add one later: ArduinoOTA.setPassword("...");

  ArduinoOTA.onStart([]() {
    const char* type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.printf("[OTA] Start updating %s\n", type);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] Update complete. Rebooting.");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    // Serial-only progress (per user preference). Print percentage every ~5%.
    static int lastPct = -1;
    int pct = (progress * 100) / total;
    if (pct != lastPct && pct % 5 == 0) {
      Serial.printf("[OTA] Progress: %d%%\n", pct);
      lastPct = pct;
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]: ", error);
    switch (error) {
      case OTA_AUTH_ERROR:    Serial.println("Auth failed"); break;
      case OTA_BEGIN_ERROR:   Serial.println("Begin failed"); break;
      case OTA_CONNECT_ERROR: Serial.println("Connect failed"); break;
      case OTA_RECEIVE_ERROR: Serial.println("Receive failed"); break;
      case OTA_END_ERROR:     Serial.println("End failed"); break;
      default:                Serial.println("Unknown"); break;
    }
  });

  ArduinoOTA.begin();
  otaInitialized = true;

  Serial.printf("[OTA] Ready. Hostname: %s.local  IP: %s\n",
                OTA_HOSTNAME, WiFi.localIP().toString().c_str());
}

void startConnect() {
  Serial.printf("[WiFi] Connecting to \"%s\"...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(OTA_HOSTNAME);
  WiFi.setSleep(false);  // keep OTA responsive; small power cost
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

}  // namespace

void begin() {
  // Register a handler so we get notified when the connection completes.
  // This lets setup() return immediately; OTA comes up asynchronously.
  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    switch (event) {
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        Serial.printf("[WiFi] Connected. IP: %s  RSSI: %d dBm\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        initializeOta();
        break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        Serial.println("[WiFi] Disconnected.");
        // Reconnect handled in loop() to avoid blocking here.
        break;
      default:
        break;
    }
  });

  startConnect();
}

void loop() {
  // Service OTA only when connected.
  if (WiFi.status() == WL_CONNECTED) {
    if (otaInitialized) {
      ArduinoOTA.handle();
    }
    return;
  }

  // Non-blocking reconnect attempt.
  unsigned long now = millis();
  if (now - lastReconnectAttempt >= RECONNECT_INTERVAL_MS) {
    lastReconnectAttempt = now;
    Serial.println("[WiFi] Retrying connection...");
    WiFi.disconnect();
    startConnect();
  }
}

bool isConnected() {
  return WiFi.status() == WL_CONNECTED;
}

}  // namespace WifiOta
