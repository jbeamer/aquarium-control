// history.h — rolling 24h of per-minute temp/heater aggregates in RAM
// Feeds the web chart without reading large CSVs from the async web task.
// After boot (once NTP has synced) the ring is rebuilt from the last two
// daily CSVs on SD, a few KB per loop() call so the main loop never stalls.

#pragma once
#include <Arduino.h>

namespace History {

constexpr size_t SLOTS = 1440;  // 24h of 1-minute buckets

struct Bucket {
  uint32_t ts;      // unix seconds, start of minute
  int16_t  tempF100;  // average temp °F × 100
  uint8_t  heaterPct; // % of samples in the minute with heater on
};

// Called for every sensor sample (live). Ignored until backfill finishes —
// those samples are already on SD and the backfill will pick them up.
void add(time_t ts, float tempF, bool heater);

// Drives the SD backfill. Call from the main loop.
void loop();

// Sequence numbers let a reader stream the ring while new minutes are added.
// Valid buckets are seq in [max(0, total()-SLOTS), total()).
uint32_t total();
bool get(uint32_t seq, Bucket& out);  // false if seq has been overwritten

}  // namespace History
