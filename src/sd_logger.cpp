// sd_logger.cpp

#include "sd_logger.h"
#include "wifi_ota.h"

#include <SD.h>
#include <sd_diskio.h>
#include <time.h>

namespace SdLogger {

namespace {

constexpr unsigned long RETRY_INTERVAL_MS = 30000;

bool sdReady = false;
int sdCs = -1;
unsigned long lastAttempt = 0;
char lastDiag[96] = "";

void dayFilename(time_t ts, char* buf, size_t len) {
  struct tm t;
  localtime_r(&ts, &t);
  snprintf(buf, len, "/%04d%02d%02d.csv",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
}

void todayFilename(char* buf, size_t len) {
  struct tm t;
  if (getLocalTime(&t, 0) && t.tm_year + 1900 >= 2024) {
    time_t now; time(&now);
    dayFilename(now, buf, len);
  } else {
    strncpy(buf, "/notime.csv", len);
  }
}

const char* cardTypeName(sdcard_type_t t) {
  switch (t) {
    case CARD_MMC:  return "MMC";
    case CARD_SD:   return "SDSC";
    case CARD_SDHC: return "SDHC/SDXC";
    case CARD_NONE: return "none";
    default:        return "unknown";
  }
}

const char* partTypeName(uint8_t t) {
  switch (t) {
    case 0x01: case 0x04: case 0x06: case 0x0E: return "FAT12/16";
    case 0x0B: case 0x0C: return "FAT32";
    case 0x07: return "exFAT/NTFS - unsupported, reformat FAT32";
    case 0xEE: return "GPT - unsupported, reformat MBR+FAT32";
    case 0x00: return "empty";
    default:   return "unknown";
  }
}

// SD.begin() only reports pass/fail. Re-run the init with the low-level
// diskio API so we can tell "card not responding" (wiring/seating) apart
// from "card OK but filesystem won't mount" (format), and peek at the MBR.
void diagnoseMountFailure(char* out, size_t len) {
  uint8_t pdrv = sdcard_init(sdCs, &SPI, 4000000);
  if (pdrv == 0xFF) {
    snprintf(out, len, "diskio driver init failed");
    return;
  }
  // f_mount(…, 1) runs the card init sequence; it fails again here, but
  // leaves the detected card type behind.
  sdcard_mount(pdrv, "/sd", 1, false);
  sdcard_type_t type = sdcard_type(pdrv);
  if (type == CARD_NONE || type == CARD_UNKNOWN) {
    snprintf(out, len, "card not responding (type=%s) - check seating/wiring",
             cardTypeName(type));
  } else {
    uint8_t sec[512];
    uint64_t mb = (uint64_t)sdcard_num_sectors(pdrv) * sdcard_sector_size(pdrv) / (1024 * 1024);
    if (!sd_read_raw(pdrv, sec, 0)) {
      snprintf(out, len, "card %s %lluMB responds, sector 0 unreadable",
               cardTypeName(type), (unsigned long long)mb);
    } else if (sec[510] != 0x55 || sec[511] != 0xAA) {
      snprintf(out, len, "card %s %lluMB, no MBR/boot signature - unformatted?",
               cardTypeName(type), (unsigned long long)mb);
    } else if (memcmp(sec + 3, "EXFAT   ", 8) == 0) {
      snprintf(out, len, "card %s %lluMB, unpartitioned exFAT - reformat FAT32",
               cardTypeName(type), (unsigned long long)mb);
    } else {
      uint8_t pt = sec[0x1C2];
      snprintf(out, len, "card %s %lluMB, partition 1 type 0x%02X (%s), FAT mount failed",
               cardTypeName(type), (unsigned long long)mb, pt, partTypeName(pt));
    }
  }
  sdcard_unmount(pdrv);
  sdcard_uninit(pdrv);
}

bool tryMount() {
  lastAttempt = millis();
  if (SD.begin(sdCs)) {
    sdReady = true;
    lastDiag[0] = '\0';
    WifiOta::logf("[SD] Ready. %s card, %llu MB total\n",
                  cardTypeName(SD.cardType()),
                  (unsigned long long)SD.totalBytes() / (1024 * 1024));
    return true;
  }
  char diag[sizeof(lastDiag)];
  diagnoseMountFailure(diag, sizeof(diag));
  // Only log when the failure mode changes, so a missing card doesn't spam
  // the log every 30s.
  if (strcmp(diag, lastDiag) != 0) {
    WifiOta::logf("[SD] Mount failed: %s (retrying every %lus)\n",
                  diag, RETRY_INTERVAL_MS / 1000);
    strncpy(lastDiag, diag, sizeof(lastDiag) - 1);
  }
  return false;
}

// A failed open usually means the card was pulled; drop to the retry loop.
void markLost(const char* path) {
  WifiOta::logf("[SD] Open failed for %s - card lost, unmounting\n", path);
  SD.end();
  sdReady = false;
  lastAttempt = millis();
}

bool openAppend(File& f, const char* path) {
  if (!sdReady) return false;
  f = SD.open(path, FILE_APPEND);
  if (!f) markLost(path);
  return (bool)f;
}

}  // namespace

void begin(int csPin) {
  sdCs = csPin;
  tryMount();
}

const char* lastError() { return lastDiag; }

void loop() {
  if (sdReady || sdCs < 0) return;
  if (millis() - lastAttempt >= RETRY_INTERVAL_MS) tryMount();
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

size_t readDayChunk(time_t day, size_t offset, char* buf, size_t maxLen) {
  if (!sdReady || maxLen == 0) return 0;
  char path[16];
  dayFilename(day, path, sizeof(path));
  if (!SD.exists(path)) return 0;
  File f = SD.open(path, FILE_READ);
  if (!f) return 0;
  size_t n = 0;
  if (offset < f.size() && f.seek(offset)) n = f.read((uint8_t*)buf, maxLen);
  f.close();
  return n;
}

}  // namespace SdLogger
