// wifi_ota.h
//
// Minimal WiFi + ArduinoOTA stub for the aquarium controller.
// Non-blocking connect with auto-reconnect, mDNS hostname, no password
// (local network trust), serial-only progress feedback.

#pragma once

#include <Arduino.h>

namespace WifiOta {

// Call once in setup(), after Serial.begin().
// Kicks off WiFi connection asynchronously; does NOT block.
void begin();

// Call every loop() iteration.
// Handles OTA upload requests and WiFi reconnection if dropped.
void loop();

// True if currently associated with the AP.
bool isConnected();

// True once SNTP has set the clock during this boot. After a soft reset
// (e.g. OTA) the RTC-derived wall clock can be tens of minutes off until
// then, while still looking plausible — gate anything that stores
// timestamps on this, not on the year.
bool timeSynced();

// Logs to Serial and any connected telnet client (port 23).
// Safe to call before WiFi connects — just writes to Serial until then.
// Connect with:  nc aquarium.local 23
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace WifiOta
