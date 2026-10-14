// web_ui.cpp

#include "web_ui.h"
#include "history.h"
#include "sd_logger.h"
#include "wifi_ota.h"

#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <SD.h>
#include <time.h>

namespace WebUi {

// ---------------------------------------------------------------------------
// Runtime settings — defaults match original #defines
// ---------------------------------------------------------------------------
float setPoint     = 78.0f;
float heaterHyst   = 0.3f;

static float statusTempF  = 0.0f;
static bool  statusTempOk = false;
int   lightsOnHour  = 9;
int   lightsOffHour = 15;
int   threshLights  = 130;
int   threshHeater  = 250;
int   threshFilter  = 110;
int      currAvgWindow      = 10;
uint32_t lastWaterChange    = 0;
int      waterChangeInterval = 14;

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------
namespace {

AsyncWebServer server(80);
AsyncEventSource events("/logs/stream");
Preferences prefs;
bool serverStarted = false;

// Ring buffer for log lines — holds last 64 entries for late-joining clients.
static constexpr int LOG_BUF_COUNT = 64;
static constexpr int LOG_BUF_LEN   = 160;
char  logRing[LOG_BUF_COUNT][LOG_BUF_LEN];
int   logHead = 0;
int   logCount = 0;

void saveSettings() {
  prefs.begin("aquarium", false);
  prefs.putFloat("setPoint",      setPoint);
  prefs.putFloat("heaterHyst",    heaterHyst);
  prefs.putInt("lightsOnHour",    lightsOnHour);
  prefs.putInt("lightsOffHour",   lightsOffHour);
  prefs.putInt("threshLights",    threshLights);
  prefs.putInt("threshHeater",    threshHeater);
  prefs.putInt("threshFilter",    threshFilter);
  prefs.putInt("currAvgWindow",    currAvgWindow);
  prefs.putUInt("lastWaterChg",    lastWaterChange);
  prefs.putInt("waterChgInterval", waterChangeInterval);
  prefs.end();
}

void loadSettings() {
  prefs.begin("aquarium", true);
  setPoint      = prefs.getFloat("setPoint",     setPoint);
  heaterHyst    = prefs.getFloat("heaterHyst",   heaterHyst);
  lightsOnHour  = prefs.getInt("lightsOnHour",   lightsOnHour);
  lightsOffHour = prefs.getInt("lightsOffHour",  lightsOffHour);
  threshLights  = prefs.getInt("threshLights",   threshLights);
  threshHeater  = prefs.getInt("threshHeater",   threshHeater);
  threshFilter  = prefs.getInt("threshFilter",   threshFilter);
  currAvgWindow       = prefs.getInt("currAvgWindow",    currAvgWindow);
  lastWaterChange     = prefs.getUInt("lastWaterChg",   lastWaterChange);
  waterChangeInterval = prefs.getInt("waterChgInterval", waterChangeInterval);
  prefs.end();
}

// ---------------------------------------------------------------------------
// HTML helpers
// ---------------------------------------------------------------------------
static const char HTML_HEAD[] PROGMEM =
  "<!DOCTYPE html><html><head>"
  "<meta charset='utf-8'>"
  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  "<title>Aquarium Controller</title>"
  "<style>"
  "body{font-family:sans-serif;background:#111;color:#ddd;margin:0;padding:16px}"
  "h1{color:#4af;margin-top:0}"
  "h2{color:#aaa;font-size:1em;text-transform:uppercase;letter-spacing:.1em}"
  "form{background:#1a1a1a;border-radius:8px;padding:16px;margin-bottom:16px}"
  "label{display:block;margin:8px 0 2px;color:#aaa;font-size:.9em}"
  "input{background:#222;border:1px solid #444;color:#fff;padding:6px 8px;"
        "border-radius:4px;width:120px}"
  "button{background:#4af;color:#000;border:none;padding:8px 20px;"
         "border-radius:4px;cursor:pointer;font-weight:bold;margin-top:12px}"
  "button:hover{background:#6cf}"
  "a{color:#4af}"
  ".row{display:flex;gap:24px;flex-wrap:wrap}"
  ".group{min-width:160px}"
  "#log{background:#0a0a0a;border-radius:8px;padding:12px;height:280px;"
       "overflow-y:auto;font-family:monospace;font-size:.8em;white-space:pre-wrap}"
  ".saved{color:#4f4;display:none}"
  "</style></head><body>"
  "<h1>Aquarium Controller</h1>"
  "<nav><a href='/'>Settings</a> &nbsp; <a href='/chart'>Chart</a>"
  " &nbsp; <a href='/logs'>Log</a></nav><br>";

static const char HTML_FOOT[] PROGMEM = "</body></html>";

// ---------------------------------------------------------------------------
// Settings page
// ---------------------------------------------------------------------------
void handleRoot(AsyncWebServerRequest* req) {
  // Pre-compute maintenance strings
  char lastChangeDateStr[48] = "Never";
  char nextDueStr[80]        = "&mdash;";
  const char* dueCssColor    = "#888";

  if (lastWaterChange > 0) {
    time_t lcTs = (time_t)lastWaterChange;
    struct tm lcTm;
    localtime_r(&lcTs, &lcTm);
    strftime(lastChangeDateStr, sizeof(lastChangeDateStr), "%a %b %d, %Y", &lcTm);

    time_t now; time(&now);
    int daysSince = (int)((now - lcTs) / 86400);
    int daysLeft  = waterChangeInterval - daysSince;

    time_t dueTs = lcTs + (time_t)waterChangeInterval * 86400;
    struct tm dueTm;
    localtime_r(&dueTs, &dueTm);
    char dueDateStr[32];
    strftime(dueDateStr, sizeof(dueDateStr), "%b %d, %Y", &dueTm);

    if (daysLeft > 1) {
      snprintf(nextDueStr, sizeof(nextDueStr), "%s (in %d days)", dueDateStr, daysLeft);
      dueCssColor = daysLeft <= 3 ? "#fda020" : "#4f4";
    } else if (daysLeft == 1) {
      snprintf(nextDueStr, sizeof(nextDueStr), "%s (tomorrow)", dueDateStr);
      dueCssColor = "#fda020";
    } else if (daysLeft == 0) {
      snprintf(nextDueStr, sizeof(nextDueStr), "%s (today)", dueDateStr);
      dueCssColor = "#fda020";
    } else {
      snprintf(nextDueStr, sizeof(nextDueStr), "%s (overdue %d day%s)",
               dueDateStr, -daysLeft, -daysLeft == 1 ? "" : "s");
      dueCssColor = "#f44";
    }
  }

  // Default the backdate field to today; max blocks future dates in the picker
  char todayStr[11] = "";
  time_t nowTs; time(&nowTs);
  struct tm nowTm;
  localtime_r(&nowTs, &nowTm);
  if (WifiOta::timeSynced()) strftime(todayStr, sizeof(todayStr), "%Y-%m-%d", &nowTm);

  static char buf[4096];
  unsigned long upSec = millis() / 1000;
  snprintf(buf, sizeof(buf),
    "%s"
    "<div style='background:#1a1a1a;border-radius:8px;padding:12px 16px;"
    "margin-bottom:16px;display:flex;gap:24px;flex-wrap:wrap;align-items:center'>"
    "<span style='color:%s;font-weight:bold'>&#9679; SD %s</span>"
    "<span style='color:#888;font-size:.85em'>%s</span>"
    "<span style='color:%s;font-weight:bold'>&#9679; WiFi %s</span>"
    "<span style='color:%s;font-weight:bold'>&#9679; Temp %s</span>"
    "<span style='color:#888'>Uptime: %luh %02lum</span>"
    "</div>"
    "<form method='post' action='/settings'>"
    "<h2>Thermostat</h2>"
    "<div class='row'>"
    "<div class='group'><label>Set point (&deg;F)</label>"
    "<input name='sp' type='number' step='0.1' value='%.1f'></div>"
    "<div class='group'><label>Hysteresis (&deg;F)</label>"
    "<input name='hy' type='number' step='0.1' value='%.1f'></div>"
    "</div>"
    "<h2>Lights Schedule (Pacific)</h2>"
    "<div class='row'>"
    "<div class='group'><label>On hour (0-23)</label>"
    "<input name='lon' type='number' min='0' max='23' value='%d'></div>"
    "<div class='group'><label>Off hour (0-23)</label>"
    "<input name='loff' type='number' min='0' max='23' value='%d'></div>"
    "</div>"
    "<h2>Current Sensor Thresholds</h2>"
    "<div class='row'>"
    "<div class='group'><label>Lights</label>"
    "<input name='tl' type='number' value='%d'></div>"
    "<div class='group'><label>Heater</label>"
    "<input name='th' type='number' value='%d'></div>"
    "<div class='group'><label>Filter</label>"
    "<input name='tf' type='number' value='%d'></div>"
    "</div>"
    "<h2>Fault Detection</h2>"
    "<div class='row'>"
    "<div class='group'><label>Averaging window (samples, 2s each)</label>"
    "<input name='caw' type='number' min='1' max='30' value='%d'></div>"
    "<div class='group'><label>Water change interval (days)</label>"
    "<input name='wci' type='number' min='1' max='365' value='%d'></div>"
    "</div>"
    "<button type='submit'>Save</button>"
    "</form>"
    "<h2>Maintenance</h2>"
    "<div style='background:#1a1a1a;border-radius:8px;padding:12px 16px;margin-bottom:16px'>"
    "<div style='margin-bottom:6px'><span style='color:#aaa'>Last change:</span> "
    "<b style='color:#ddd'>%s</b></div>"
    "<div><span style='color:#aaa'>Next due:</span> "
    "<b style='color:%s'>%s</b></div>"
    "</div>"
    "<form method='post' action='/waterchange' style='margin-bottom:16px;"
    "display:flex;gap:8px;align-items:center;flex-wrap:wrap'>"
    "<input name='date' type='date' value='%s' max='%s' style='width:auto'>"
    "<button type='submit' style='background:#2a6;color:#fff;margin-top:0'>"
    "Log Water Change</button>"
    "</form>"
    "%s",
    HTML_HEAD,
    SdLogger::isReady() ? "#4f4" : "#f44", SdLogger::isReady() ? "Ready" : "Not mounted", SdLogger::lastError(),
    WifiOta::isConnected() ? "#4f4" : "#f44", WifiOta::isConnected() ? "Connected" : "Disconnected",
    statusTempOk ? "#4f4" : "#f44", statusTempOk ? "OK" : "Fault",
    upSec / 3600, (upSec % 3600) / 60,
    setPoint, heaterHyst,
    lightsOnHour, lightsOffHour,
    threshLights, threshHeater, threshFilter,
    currAvgWindow, waterChangeInterval,
    lastChangeDateStr, dueCssColor, nextDueStr,
    todayStr, todayStr,
    HTML_FOOT);
  req->send(200, "text/html", buf);
}

void handleSettingsPost(AsyncWebServerRequest* req) {
  if (req->hasParam("sp",   true)) setPoint      = req->getParam("sp",   true)->value().toFloat();
  if (req->hasParam("hy",   true)) heaterHyst    = req->getParam("hy",   true)->value().toFloat();
  if (req->hasParam("lon",  true)) lightsOnHour  = req->getParam("lon",  true)->value().toInt();
  if (req->hasParam("loff", true)) lightsOffHour = req->getParam("loff", true)->value().toInt();
  if (req->hasParam("tl",   true)) threshLights  = req->getParam("tl",   true)->value().toInt();
  if (req->hasParam("th",   true)) threshHeater  = req->getParam("th",   true)->value().toInt();
  if (req->hasParam("tf",   true)) threshFilter  = req->getParam("tf",   true)->value().toInt();
  if (req->hasParam("caw",  true)) currAvgWindow       = constrain(req->getParam("caw", true)->value().toInt(), 1, 30);
  if (req->hasParam("wci",  true)) waterChangeInterval = constrain(req->getParam("wci", true)->value().toInt(), 1, 365);
  saveSettings();
  WifiOta::logf("[Web] Settings saved: sp=%.1f hy=%.1f lon=%d loff=%d tl=%d th=%d tf=%d caw=%d wci=%d\n",
                setPoint, heaterHyst, lightsOnHour, lightsOffHour,
                threshLights, threshHeater, threshFilter, currAvgWindow, waterChangeInterval);
  req->redirect("/");
}

// ---------------------------------------------------------------------------
// Log viewer page (SSE-driven)
// ---------------------------------------------------------------------------
void handleLogsPage(AsyncWebServerRequest* req) {
  char buf[2048];
  snprintf(buf, sizeof(buf),
    "%s"
    "<h2>Live Log</h2>"
    "<div id='log'></div>"
    "<script>"
    "var log=document.getElementById('log');"
    "var es=new EventSource('/logs/stream');"
    "es.onmessage=function(e){"
    "  log.textContent+=e.data+'\\n';"
    "  log.scrollTop=log.scrollHeight;"
    "};"
    "</script>"
    "%s",
    HTML_HEAD, HTML_FOOT);
  req->send(200, "text/html", buf);
}

// ---------------------------------------------------------------------------
// Chart page
// ---------------------------------------------------------------------------
void handleChartPage(AsyncWebServerRequest* req) {
  static char buf[4096];
  snprintf(buf, sizeof(buf),
    "%s"
    "<h2>Temperature &amp; Heater</h2>"
    "<canvas id='c' style='width:100%%;max-width:900px;background:#1a1a1a;"
    "border-radius:8px'></canvas>"
    "<p id='nodata' style='display:none;color:#888'>No history yet "
    "(first minute after boot, or SD not mounted).</p>"
    "<script src='https://cdn.jsdelivr.net/npm/chart.js@4/dist/chart.umd.min.js'></script>"
    "<script src='https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns@3/dist/chartjs-adapter-date-fns.bundle.min.js'></script>"
    "<script>"
    "fetch('/data').then(r=>r.json()).then(d=>{"
    "  var temps=d.pts.map(p=>({x:p[0]*1000,y:p[1]})),"
    "      heater=d.pts.map(p=>({x:p[0]*1000,y:p[2]/100}));"
    "  if(!temps.length){document.getElementById('c').style.display='none';"
    "    document.getElementById('nodata').style.display='block';return;}"
    "  var ctx=document.getElementById('c').getContext('2d');"
    "  new Chart(ctx,{"
    "    data:{"
    "      datasets:[{"
    "        type:'line',label:'Temp (F)',data:temps,"
    "        borderColor:'#4af',borderWidth:1.5,pointRadius:0,"
    "        yAxisID:'y'"
    "      },{"
    "        type:'bar',label:'Heater duty',data:heater,"
    "        backgroundColor:'rgba(253,160,32,0.25)',borderColor:'rgba(253,160,32,0.5)',"
    "        borderWidth:1,yAxisID:'y2',barPercentage:1,categoryPercentage:1"
    "      }]"
    "    },"
    "    options:{"
    "      animation:false,responsive:true,"
    "      scales:{"
    "        x:{type:'time',time:{unit:'hour',displayFormats:{hour:'HH:mm'}},"
    "           ticks:{color:'#888'},grid:{color:'#222'}},"
    "        y:{position:'left',suggestedMin:%.1f,suggestedMax:%.1f,"
    "           ticks:{color:'#adf'},grid:{color:'#222'},"
    "           title:{display:true,text:'Temp F',color:'#888'}},"
    "        y2:{position:'right',min:0,max:1,ticks:{display:false},"
    "            grid:{display:false}}"
    "      },"
    "      plugins:{legend:{labels:{color:'#ccc'}}}"
    "    }"
    "  });"
    "});"
    "</script>"
    "%s",
    HTML_HEAD, setPoint - 2.0f, setPoint + 2.0f, HTML_FOOT);
  req->send(200, "text/html", buf);
}

// ---------------------------------------------------------------------------
// /data — per-minute history as {"pts":[[unix_s,tempF,heaterPct],...]}
// Streamed from the RAM ring in chunks; never touches SD from this task.
// ---------------------------------------------------------------------------
void handleData(AsyncWebServerRequest* req) {
  struct Cursor { uint32_t seq, end; uint8_t phase; bool any; };
  auto cur = std::make_shared<Cursor>();
  cur->end = History::total();
  cur->seq = cur->end > History::SLOTS ? cur->end - History::SLOTS : 0;
  cur->phase = 0;
  cur->any = false;

  AsyncWebServerResponse* res = req->beginChunkedResponse("application/json",
    [cur](uint8_t* out, size_t maxLen, size_t) -> size_t {
      constexpr size_t PT_MAX = 32;  // "[4294967295,-327.68,100],"
      size_t n = 0;
      if (cur->phase == 0) {
        if (maxLen < 8) return 0;
        n += snprintf((char*)out, maxLen, "{\"pts\":[");
        cur->phase = 1;
      }
      if (cur->phase == 1) {
        while (cur->seq < cur->end && n + PT_MAX < maxLen) {
          History::Bucket b;
          if (History::get(cur->seq++, b)) {
            n += snprintf((char*)out + n, maxLen - n, "%s[%lu,%.2f,%u]",
                          cur->any ? "," : "", (unsigned long)b.ts,
                          b.tempF100 / 100.0f, b.heaterPct);
            cur->any = true;
          }
        }
        if (cur->seq >= cur->end && n + 3 < maxLen) {
          n += snprintf((char*)out + n, maxLen - n, "]}");
          cur->phase = 2;
        }
        return n;
      }
      return 0;  // phase 2: done
    });
  req->send(res);
}

// Optional 'date' field (YYYY-MM-DD) backdates the change; recorded at local
// noon so the day count isn't skewed by the time of day. Empty/missing = now.
void handleWaterChangePost(AsyncWebServerRequest* req) {
  time_t when = 0;
  if (req->hasParam("date", true)) {
    String d = req->getParam("date", true)->value();
    if (d.length() > 0) {
      struct tm t = {};
      if (sscanf(d.c_str(), "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday) != 3) {
        req->send(400, "text/plain", "Bad date");
        return;
      }
      t.tm_year -= 1900;
      t.tm_mon  -= 1;
      t.tm_hour  = 12;
      t.tm_isdst = -1;
      when = mktime(&t);
    }
  }
  if (!logWaterChange(when)) {
    req->send(400, "text/plain", "Not logged: clock not synced or date out of range");
    return;
  }
  req->redirect("/");
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void begin() {
  loadSettings();

  server.on("/",           HTTP_GET,  handleRoot);
  server.on("/settings",   HTTP_POST, handleSettingsPost);
  server.on("/waterchange",HTTP_POST, handleWaterChangePost);
  server.on("/logs",       HTTP_GET,  handleLogsPage);
  server.on("/chart",      HTTP_GET,  handleChartPage);
  server.on("/data",       HTTP_GET,  handleData);
  // Raw daily CSVs, e.g. /sd/20261013.csv (streamed in small chunks by the library)
  server.serveStatic("/sd/", SD, "/");

  events.onConnect([](AsyncEventSourceClient* client) {
    // Replay ring buffer to new client
    int start = (logCount < LOG_BUF_COUNT) ? 0 : logHead;
    int count = min(logCount, LOG_BUF_COUNT);
    for (int i = 0; i < count; i++) {
      int idx = (start + i) % LOG_BUF_COUNT;
      client->send(logRing[idx], nullptr, millis());
    }
  });
  server.addHandler(&events);
  server.begin();
  serverStarted = true;
  WifiOta::logf("[Web] HTTP server on port 80  http://aquarium.local/\n");
}

void loop() {
  // AsyncWebServer handles everything in callbacks — nothing needed here.
  // Kept for symmetry with WifiOta::loop().
}

void setTempStatus(float tempF, bool valid) {
  statusTempF  = tempF;
  statusTempOk = valid;
}

bool logWaterChange(time_t when) {
  time_t now; time(&now);
  if (!WifiOta::timeSynced()) {
    WifiOta::logf("[Maint] Water change NOT logged: clock not synced\n");
    return false;
  }
  if (when == 0) when = now;
  // Allow a day of slack for TZ/noon rounding on today's date
  if (when < 1704067200 /* 2024-01-01 */ || when > now + 86400) {
    WifiOta::logf("[Maint] Water change NOT logged: date out of range\n");
    return false;
  }
  lastWaterChange = (uint32_t)when;
  saveSettings();
  struct tm wt;
  localtime_r(&when, &wt);
  char ds[24];
  strftime(ds, sizeof(ds), "%Y-%m-%d %H:%M", &wt);
  WifiOta::logf("[Maint] Water change logged: %s\n", ds);
  return true;
}

void pushLog(const char* msg) {
  if (!serverStarted) return;
  // Store in ring buffer
  strncpy(logRing[logHead], msg, LOG_BUF_LEN - 1);
  logRing[logHead][LOG_BUF_LEN - 1] = '\0';
  // Strip trailing newline for SSE (message boundary is the \n\n frame)
  int len = strlen(logRing[logHead]);
  if (len > 0 && logRing[logHead][len - 1] == '\n') logRing[logHead][len - 1] = '\0';
  logHead = (logHead + 1) % LOG_BUF_COUNT;
  if (logCount < LOG_BUF_COUNT) logCount++;

  // Push to connected SSE clients
  events.send(msg, nullptr, millis());
}

}  // namespace WebUi
