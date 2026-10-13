// sd_logger.h — daily CSV logging to SD card
// Files: /YYYYMMDD.csv, one row per sensor sample + relay-change events.
// CSV columns: unix_ts, type, temp_f, heater, lights, filter, pkpk_l, pkpk_h, pkpk_f
// type: S=sample, E=event (relay change). Event rows omit sensor columns.

#pragma once
#include <Arduino.h>

namespace SdLogger {

void begin(int csPin);
void loop();  // retries the mount every 30s while unmounted
bool isReady();
const char* lastError();  // last mount-failure diagnosis, "" when mounted

// Called every SENSOR_INTERVAL — logs a full sensor sample row.
void logSample(time_t ts, float tempF,
               bool heater, bool lights, bool filter,
               int pkpkL, int pkpkH, int pkpkF);

// Called on relay state changes — logs a brief event row.
void logEvent(time_t ts, const char* msg);

// Reads up to maxLen bytes from the CSV for the local day containing `day`,
// starting at `offset`. Returns bytes read; 0 at EOF or if the file is missing.
// Not NUL-terminated. Used by History to rebuild the chart after boot.
size_t readDayChunk(time_t day, size_t offset, char* buf, size_t maxLen);

}  // namespace SdLogger
