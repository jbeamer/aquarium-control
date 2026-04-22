// web_ui.cpp

#include "web_ui.h"
#include "sd_logger.h"
#include "wifi_ota.h"

#include <ESPAsyncWebServer.h>
#include <Preferences.h>
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
int   currAvgWindow = 10;

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
  prefs.putInt("currAvgWindow",   currAvgWindow);
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
  currAvgWindow = prefs.getInt("currAvgWindow",  currAvgWindow);
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
  char buf[3200];
  unsigned long upSec = millis() / 1000;
  snprintf(buf, sizeof(buf),
    "%s"
    "<div style='background:#1a1a1a;border-radius:8px;padding:12px 16px;"
    "margin-bottom:16px;display:flex;gap:24px;flex-wrap:wrap;align-items:center'>"
    "<span style='color:%s;font-weight:bold'>&#9679; SD %s</span>"
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
    "</div>"
    "<button type='submit'>Save</button>"
    "</form>"
    "%s",
    HTML_HEAD,
    SdLogger::isReady() ? "#4f4" : "#f44", SdLogger::isReady() ? "Ready" : "Not mounted",
    WifiOta::isConnected() ? "#4f4" : "#f44", WifiOta::isConnected() ? "Connected" : "Disconnected",
    statusTempOk ? "#4f4" : "#f44", statusTempOk ? "OK" : "Fault",
    upSec / 3600, (upSec % 3600) / 60,
    setPoint, heaterHyst,
    lightsOnHour, lightsOffHour,
    threshLights, threshHeater, threshFilter,
    currAvgWindow,
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
  if (req->hasParam("caw",  true)) currAvgWindow = constrain(req->getParam("caw", true)->value().toInt(), 1, 30);
  saveSettings();
  WifiOta::logf("[Web] Settings saved: sp=%.1f hy=%.1f lon=%d loff=%d tl=%d th=%d tf=%d caw=%d\n",
                setPoint, heaterHyst, lightsOnHour, lightsOffHour,
                threshLights, threshHeater, threshFilter, currAvgWindow);
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
  char buf[3000];
  snprintf(buf, sizeof(buf),
    "%s"
    "<h2>Temperature &amp; Heater</h2>"
    "<canvas id='c' style='width:100%%;max-width:900px;background:#1a1a1a;"
    "border-radius:8px'></canvas>"
    "<script src='https://cdn.jsdelivr.net/npm/chart.js@4/dist/chart.umd.min.js'></script>"
    "<script src='https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns@3/dist/chartjs-adapter-date-fns.bundle.min.js'></script>"
    "<script>"
    "fetch('/data').then(r=>r.json()).then(d=>{"
    "  var ctx=document.getElementById('c').getContext('2d');"
    "  new Chart(ctx,{"
    "    data:{"
    "      datasets:[{"
    "        type:'line',label:'Temp (F)',data:d.temps,"
    "        borderColor:'#4af',borderWidth:1.5,pointRadius:0,"
    "        yAxisID:'y'"
    "      },{"
    "        type:'bar',label:'Heater',data:d.heater,"
    "        backgroundColor:'rgba(253,160,32,0.25)',borderColor:'rgba(253,160,32,0.5)',"
    "        borderWidth:1,yAxisID:'y2',barPercentage:1,categoryPercentage:1"
    "      }]"
    "    },"
    "    options:{"
    "      animation:false,responsive:true,"
    "      scales:{"
    "        x:{type:'time',time:{unit:'minute',displayFormats:{minute:'HH:mm'}},"
    "           ticks:{color:'#888'},grid:{color:'#222'}},"
    "        y:{position:'left',ticks:{color:'#adf'},grid:{color:'#222'},"
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
    HTML_HEAD, HTML_FOOT);
  req->send(200, "text/html", buf);
}

// ---------------------------------------------------------------------------
// /data — JSON for chart (reads today's CSV from SD)
// ---------------------------------------------------------------------------
void handleData(AsyncWebServerRequest* req) {
  // Read CSV, parse into JSON arrays for Chart.js
  // {temps:[{x:ms,y:f},...], heater:[{x:ms,y:0or1},...]}
  static char csvBuf[32768];
  size_t n = SdLogger::readTodayCsv(csvBuf, sizeof(csvBuf));

  String out = "{\"temps\":[";
  String heaterOut = "],\"heater\":[";
  bool firstT = true, firstH = true;

  char* line = strtok(csvBuf, "\n");
  while (line) {
    long long ts;
    char type[4];
    float tempF;
    int heater, lights, filter, pkl, pkh, pkf;
    int parsed = sscanf(line, "%lld,%3[^,],%f,%d,%d,%d,%d,%d,%d",
                        &ts, type, &tempF, &heater, &lights, &filter, &pkl, &pkh, &pkf);
    if (parsed == 9 && type[0] == 'S') {
      long long ms = ts * 1000LL;
      if (!firstT) out += ',';
      char pt[48];
      snprintf(pt, sizeof(pt), "{\"x\":%lld,\"y\":%.2f}", ms, tempF);
      out += pt;
      firstT = false;

      if (!firstH) heaterOut += ',';
      snprintf(pt, sizeof(pt), "{\"x\":%lld,\"y\":%d}", ms, heater);
      heaterOut += pt;
      firstH = false;
    }
    line = strtok(nullptr, "\n");
  }

  out += heaterOut;
  out += "]}";
  req->send(200, "application/json", out);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void begin() {
  loadSettings();

  server.on("/",         HTTP_GET,  handleRoot);
  server.on("/settings", HTTP_POST, handleSettingsPost);
  server.on("/logs",     HTTP_GET,  handleLogsPage);
  server.on("/chart",    HTTP_GET,  handleChartPage);
  server.on("/data",     HTTP_GET,  handleData);

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
