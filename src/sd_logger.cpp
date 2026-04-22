// sd_logger.cpp

#include "sd_logger.h"
#include "wifi_ota.h"

#include <SD.h>
#include <time.h>

namespace SdLogger {

namespace {

bool sdReady = false;
int sdCs = -1;

void todayFilename(char* buf, size_t len) {
  struct tm t;
  if (getLocalTime(&t, 0) && t.tm_year + 1900 >= 2024) {
    snprintf(buf, len, "/%04d%02d%02d.csv",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  } else {
    strncpy(buf, "/notime.csv", len);
  }
}

bool openAppend(File& f, const char* path) {
  if (!sdReady) return false;
  f = SD.open(path, FILE_APPEND);
  return (bool)f;
}

}  // namespace

void begin(int csPin) {
  sdCs = csPin;
  if (!SD.begin(csPin)) {
    WifiOta::logf("[SD] Mount failed — no card or wiring issue\n");
    return;
  }
  sdReady = true;
  WifiOta::logf("[SD] Ready. %llu MB total\n",
                (unsigned long long)SD.totalBytes() / (1024 * 1024));
}

bool isReady() { return sdReady; }

void logSample(time_t ts, float tempF,
               bool heater, bool lights, bool filter,
               int pkpkL, int pkpkH, int pkpkF) {
  if (!sdReady) return;
  char path[16];
  todayFilename(path, sizeof(path));
  File f;
  if (!openAppend(f, path)) return;
  f.printf("%lld,S,%.2f,%d,%d,%d,%d,%d,%d\n",
           (long long)ts, tempF,
           (int)heater, (int)lights, (int)filter,
           pkpkL, pkpkH, pkpkF);
  f.close();
}

void logEvent(time_t ts, const char* msg) {
  if (!sdReady) return;
  char path[16];
  todayFilename(path, sizeof(path));
  File f;
  if (!openAppend(f, path)) return;
  f.printf("%lld,E,%s\n", (long long)ts, msg);
  f.close();
}

size_t readTodayCsv(char* buf, size_t maxLen) {
  if (!sdReady || maxLen == 0) return 0;
  char path[16];
  todayFilename(path, sizeof(path));
  File f = SD.open(path, FILE_READ);
  if (!f) return 0;
  size_t n = f.read((uint8_t*)buf, maxLen - 1);
  buf[n] = '\0';
  f.close();
  return n;
}

}  // namespace SdLogger
