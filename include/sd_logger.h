// sd_logger.h — daily CSV logging to SD card
// Files: /YYYYMMDD.csv, one row per sensor sample + relay-change events.
// CSV columns: unix_ts, type, temp_f, heater, lights, filter, pkpk_l, pkpk_h, pkpk_f
// type: S=sample, E=event (relay change). Event rows omit sensor columns.

#pragma once
#include <Arduino.h>

namespace SdLogger {

void begin(int csPin);
bool isReady();

// Called every SENSOR_INTERVAL — logs a full sensor sample row.
void logSample(time_t ts, float tempF,
               bool heater, bool lights, bool filter,
               int pkpkL, int pkpkH, int pkpkF);

// Called on relay state changes — logs a brief event row.
void logEvent(time_t ts, const char* msg);

// Fills buf with up to maxLen bytes of today's CSV file content.
// Returns number of bytes written. Used by web /data endpoint.
size_t readTodayCsv(char* buf, size_t maxLen);

}  // namespace SdLogger
