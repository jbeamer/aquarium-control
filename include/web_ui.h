// web_ui.h — async HTTP server: settings, SSE log stream, chart + data

#pragma once
#include <Arduino.h>

namespace WebUi {

void begin();
void loop();

// Push a log line to any connected SSE clients. Called from WifiOta::logf.
void pushLog(const char* msg);

// Called from main loop to keep status bar current.
void setTempStatus(float tempF, bool valid);

// Runtime-configurable settings (loaded from Preferences at boot).
extern float  setPoint;
extern float  heaterHyst;
extern int    lightsOnHour;
extern int    lightsOffHour;
extern int    threshLights;
extern int    threshHeater;
extern int    threshFilter;
extern int      currAvgWindow;       // samples to average for fault detection (2s each)
extern uint32_t lastWaterChange;     // unix epoch of last logged change, 0 = never
extern int      waterChangeInterval; // days between changes (default 14)

void logWaterChange(); // record now as last water change and persist to NVS

}  // namespace WebUi
