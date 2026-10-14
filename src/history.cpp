// history.cpp

#include "history.h"
#include "sd_logger.h"
#include "wifi_ota.h"

#include <time.h>

namespace History {

namespace {

constexpr size_t CHUNK = 4096;
constexpr time_t WINDOW_S = SLOTS * 60;

Bucket ring[SLOTS];
volatile uint32_t written = 0;  // total buckets ever committed (seq of next)

// In-progress minute
uint32_t accMinute = 0;
float    accSum = 0;
uint16_t accN = 0, accHeat = 0;

enum BackfillState : uint8_t { BF_WAITING, BF_RUNNING, BF_DONE };
BackfillState bfState = BF_WAITING;
time_t   bfDay = 0;       // noon of the day whose file we're reading
size_t   bfPos = 0;       // byte offset into that file
char     bfLine[96];      // carries a partial line across chunks
size_t   bfLineLen = 0;
uint32_t bfRows = 0;
unsigned long bfStartMs = 0;

void commit() {
  if (accN == 0) return;
  Bucket& b = ring[written % SLOTS];
  b.ts = accMinute * 60;
  b.tempF100 = (int16_t)lroundf(accSum / accN * 100.0f);
  b.heaterPct = (uint8_t)(accHeat * 100 / accN);
  written = written + 1;
  accN = 0; accSum = 0; accHeat = 0;
}

void aggregate(time_t ts, float tempF, bool heater) {
  if (tempF < 32.0f || tempF > 120.0f) return;  // sensor fault readings
  uint32_t minute = (uint32_t)(ts / 60);
  if (minute < accMinute) {
    // Clock stepped backwards: latest time wins. Drop the in-progress minute
    // and any committed buckets that are now in the future, rather than
    // discarding every sample until the clock catches back up.
    accN = 0; accSum = 0; accHeat = 0;
    while (written > 0 && ring[(written - 1) % SLOTS].ts >= minute * 60) {
      written = written - 1;
    }
    accMinute = minute;
  }
  if (minute != accMinute) {
    commit();
    accMinute = minute;
  }
  accSum += tempF;
  accN++;
  if (heater) accHeat++;
}

time_t localNoon(time_t ts) {
  struct tm t;
  localtime_r(&ts, &t);
  t.tm_hour = 12; t.tm_min = 0; t.tm_sec = 0; t.tm_isdst = -1;
  return mktime(&t);
}

void parseLine(const char* line, time_t cutoff) {
  long long ts;
  char type;
  float tempF;
  int heater;
  if (sscanf(line, "%lld,%c,%f,%d", &ts, &type, &tempF, &heater) != 4) return;
  if (type != 'S' || ts < cutoff) return;
  aggregate((time_t)ts, tempF, heater != 0);
  bfRows++;
}

void finishBackfill() {
  bfState = BF_DONE;
  WifiOta::logf("[History] Backfill done: %lu rows -> %lu minutes in %lums\n",
                (unsigned long)bfRows, (unsigned long)written,
                millis() - bfStartMs);
}

}  // namespace

void add(time_t ts, float tempF, bool heater) {
  if (bfState != BF_DONE || !WifiOta::timeSynced()) return;
  aggregate(ts, tempF, heater);
}

void loop() {
  if (bfState == BF_DONE) return;

  if (!WifiOta::timeSynced()) return;
  time_t now; time(&now);

  if (bfState == BF_WAITING) {
    bfStartMs = millis();
    if (!SdLogger::isReady()) {
      WifiOta::logf("[History] SD not mounted - chart starts empty\n");
      bfState = BF_DONE;
      return;
    }
    bfDay = localNoon(now - WINDOW_S);
    bfPos = 0; bfLineLen = 0;
    bfState = BF_RUNNING;
  }

  // One chunk per call. Re-opening each time means rows appended by the
  // live logger while we work are picked up before we hand over to add().
  static char buf[CHUNK];
  size_t n = SdLogger::readDayChunk(bfDay, bfPos, buf, sizeof(buf));
  if (n == 0) {
    // End of this day's file (or no file). Advance, or finish at today.
    if (bfDay >= localNoon(now)) {
      if (bfLineLen) { bfLine[bfLineLen] = '\0'; parseLine(bfLine, now - WINDOW_S); }
      finishBackfill();
      return;
    }
    bfDay = localNoon(bfDay + 86400);
    bfPos = 0; bfLineLen = 0;
    return;
  }
  bfPos += n;

  time_t cutoff = now - WINDOW_S;
  for (size_t i = 0; i < n; i++) {
    char c = buf[i];
    if (c == '\n') {
      bfLine[bfLineLen] = '\0';
      parseLine(bfLine, cutoff);
      bfLineLen = 0;
    } else if (bfLineLen < sizeof(bfLine) - 1) {
      bfLine[bfLineLen++] = c;
    }
  }
}

uint32_t total() { return written; }

bool get(uint32_t seq, Bucket& out) {
  uint32_t w = written;
  if (seq >= w || (w > SLOTS && seq < w - SLOTS)) return false;
  out = ring[seq % SLOTS];
  // Re-check: the slot may have been overwritten while we copied it
  return !(written > SLOTS && seq < written - SLOTS);
}

}  // namespace History
