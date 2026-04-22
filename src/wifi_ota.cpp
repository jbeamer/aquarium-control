// wifi_ota.cpp

#include "wifi_ota.h"
#include "web_ui.h"
#include "config.h"

#include <WiFi.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <stdarg.h>

namespace WifiOta {

namespace {

constexpr unsigned long RECONNECT_INTERVAL_MS = 10000;
constexpr int RECONNECT_REBOOT_AFTER = 30;  // 30 × 10s = 5 min
unsigned long lastReconnectAttempt = 0;
int reconnectAttempts = 0;
bool otaInitialized = false;

// ---------------------------------------------------------------------------
// Telnet mirror — one client at a time on port 23
// ---------------------------------------------------------------------------
WiFiServer telnetServer(23);
WiFiClient telnetClient;
bool telnetStarted = false;

void startTelnet() {
  if (telnetStarted) return;
  telnetServer.begin();
  telnetServer.setNoDelay(true);
  telnetStarted = true;
  Serial.println("[Telnet] Listening on port 23  (nc aquarium.local 23)");
}

void checkTelnetClients() {
  if (!telnetStarted) return;

  if (telnetServer.hasClient()) {
    WiFiClient newClient = telnetServer.accept();
    if (telnetClient && telnetClient.connected()) {
      // Boot existing client before accepting new one
      telnetClient.println("\r\n[Telnet] Replaced by new connection.");
      telnetClient.stop();
    }
    telnetClient = newClient;
    telnetClient.printf("=== Aquarium Controller  %s ===\r\n",
                        WiFi.localIP().toString().c_str());
    Serial.println("[Telnet] Client connected");
  }

  // Discard incoming bytes (IAC negotiations, accidental keystrokes)
  while (telnetClient && telnetClient.available()) telnetClient.read();
}

// ---------------------------------------------------------------------------
// OTA
// ---------------------------------------------------------------------------
void initializeOta() {
  if (otaInitialized) return;

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  // No password — relying on local network trust.

  ArduinoOTA.onStart([]() {
    const char* type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    logf("[OTA] Start updating %s\n", type);
  });

  ArduinoOTA.onEnd([]() {
    logf("[OTA] Update complete. Rebooting.\n");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static int lastPct = -1;
    int pct = (progress * 100) / total;
    if (pct != lastPct && pct % 5 == 0) {
      logf("[OTA] Progress: %d%%\n", pct);
      lastPct = pct;
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    const char* msg;
    switch (error) {
      case OTA_AUTH_ERROR:    msg = "Auth failed";    break;
      case OTA_BEGIN_ERROR:   msg = "Begin failed";   break;
      case OTA_CONNECT_ERROR: msg = "Connect failed"; break;
      case OTA_RECEIVE_ERROR: msg = "Receive failed"; break;
      case OTA_END_ERROR:     msg = "End failed";     break;
      default:                msg = "Unknown";        break;
    }
    logf("[OTA] Error[%u]: %s\n", error, msg);
  });

  ArduinoOTA.begin();
  otaInitialized = true;
  logf("[OTA] Ready. Hostname: %s.local  IP: %s\n",
       OTA_HOSTNAME, WiFi.localIP().toString().c_str());
}

void startConnect() {
  logf("[WiFi] Connecting to \"%s\"...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(OTA_HOSTNAME);
  WiFi.setSleep(false);        // keep OTA responsive
  WiFi.setAutoReconnect(true); // let the stack reconnect without blocking loop
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void begin() {
  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    switch (event) {
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        reconnectAttempts = 0;
        logf("[WiFi] Connected. IP: %s  RSSI: %d dBm\n",
             WiFi.localIP().toString().c_str(), WiFi.RSSI());
        startTelnet();
        initializeOta();
        break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        logf("[WiFi] Disconnected.\n");
        break;
      default:
        break;
    }
  });

  startConnect();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    checkTelnetClients();
    if (otaInitialized) ArduinoOTA.handle();
    return;
  }

  unsigned long now = millis();
  if (now - lastReconnectAttempt >= RECONNECT_INTERVAL_MS) {
    lastReconnectAttempt = now;
    reconnectAttempts++;
    if (reconnectAttempts >= RECONNECT_REBOOT_AFTER) {
      logf("[WiFi] No connection after %d attempts — rebooting\n", reconnectAttempts);
      delay(200);
      ESP.restart();
    }
    logf("[WiFi] Retrying connection... (attempt %d)\n", reconnectAttempts);
    startConnect();
  }
}

bool isConnected() {
  return WiFi.status() == WL_CONNECTED;
}

void logf(const char* fmt, ...) {
  char buf[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  Serial.print(buf);

  if (telnetClient && telnetClient.connected()) {
    telnetClient.print(buf);
  }

  WebUi::pushLog(buf);
}

}  // namespace WifiOta
