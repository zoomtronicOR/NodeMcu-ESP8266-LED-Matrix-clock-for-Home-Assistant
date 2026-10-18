#include "web.h"

#include <ArduinoJson.h>
#if defined(ESP8266)
#include <ESP8266WebServer.h>
using WebServerT = ESP8266WebServer;
#else
#include <Update.h>
#include <WebServer.h>
using WebServerT = WebServer;
#endif

#include <memory>

#include "app.h"
#include "config.h"
#include "diag.h"
#include "display.h"
#include "generated/web_index.h"
#include "mqtt.h"
#include "net.h"
#include "ota.h"
#include "platform.h"
#include "sensors.h"
#include "system.h"
#include "text.h"
#include "timesync.h"
#include "weather.h"

constexpr size_t MAX_BODY = 4096;      // najveće prihvaćeno telo zahteva
constexpr size_t MAX_RENDER_TEXT = 64; // pregled fonta
constexpr uint16_t MAX_RENDER_COLS = 400;

static WebServerT server(80);

// stanje web OTA (upload callback se poziva po delovima, pa završni handler)
static bool otaActive = false;
static bool otaDone = false;
static char otaErr[96];

// ---------------------------------------------------------------- pomoćne

static const char* updateError() {
#if defined(ESP8266)
  static String e;
  e = Update.getErrorString();
  return e.c_str();
#else
  return Update.errorString();
#endif
}

static bool authorized() {
  if (!cfg.adminPass[0]) return true;
  if (server.authenticate("admin", cfg.adminPass)) return true;
  server.requestAuthentication(BASIC_AUTH, "Matricni sat");
  return false;
}

static void sendJson(int code, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(code, F("application/json"), out);
}

static void sendError(int code, const char* msg) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = msg;
  sendJson(code, doc);
}

static void sendOk() {
  JsonDocument doc;
  doc["ok"] = true;
  sendJson(200, doc);
}

// čita JSON telo zahteva uz proveru veličine; false = odgovor sa greškom je već poslat
static bool readBody(JsonDocument& doc) {
  const String& body = server.arg("plain");
  if (body.length() == 0) {
    sendError(400, "prazan zahtev");
    return false;
  }
  if (body.length() > MAX_BODY) {
    sendError(413, "zahtev je prevelik");
    return false;
  }
  DeserializationError e = deserializeJson(doc, body);
  if (e) {
    sendError(400, "neispravan JSON");
    return false;
  }
  if (!doc.is<JsonObject>()) {
    sendError(400, "očekivan JSON objekat");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- rute

static void handleIndex() {
  if (!authorized()) return;
  server.sendHeader(F("Content-Encoding"), F("gzip"));
  server.sendHeader(F("Cache-Control"), F("no-cache"));
  server.send_P(200, PSTR("text/html; charset=utf-8"), (PGM_P)WEB_INDEX_GZ, WEB_INDEX_GZ_LEN);
}

static void handleStatus() {
  if (!authorized()) return;
  JsonDocument doc;
  doc["fw"] = FW_VERSION;
  doc["build"] = FW_BUILD;

  if (timeValid()) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char buf[24];
    snprintf_P(buf, sizeof(buf), PSTR("%02d:%02d:%02d"), t.tm_hour, t.tm_min, t.tm_sec);
    doc["time"] = buf;
    snprintf_P(buf, sizeof(buf), PSTR("%02d.%02d.%04d."), t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
    doc["date"] = buf;
  }
  TimeStatus ts = timeStatus();
  doc["ntp"] = timeStatusText(ts);
  doc["ntpOk"] = ts == TIME_SYNCED;
  doc["ntpAge"] = timeLastSync() ? (long)(time(nullptr) - timeLastSync()) : -1;
  doc["ntpCount"] = timeSyncCount();

  JsonObject w = doc["wifi"].to<JsonObject>();
  w["state"] = netStateText();
  w["ok"] = netConnected();
  w["ssid"] = cfg.wifiSsid;
  if (netConnected()) {
    w["ip"] = WiFi.localIP().toString();
    w["rssi"] = WiFi.RSSI();
    w["ch"] = WiFi.channel();
  }
  w["host"] = cfg.hostname;
  w["fails"] = netFailCount();
  w["disc"] = netDisconnectCount();
  w["mac"] = WiFi.macAddress();
  JsonObject p = doc["portal"].to<JsonObject>();
  p["on"] = portalActive();
  p["ssid"] = portalSsid();

  JsonObject m = doc["mqtt"].to<JsonObject>();
  m["on"] = cfg.mqtt.enabled;
  m["state"] = mqttStateText();
  m["ok"] = mqttConnected();
  m["err"] = mqttLastError();
  m["connects"] = mqttConnectCount();
  m["fails"] = mqttFailCount();
  m["age"] = mqttLastConnect() ? (long)(time(nullptr) - mqttLastConnect()) : -1;
  m["base"] = cfg.mqtt.base;

  JsonObject se = doc["sens"].to<JsonObject>();
  se["type"] = sensorActiveType();
  se["name"] = sensorTypeName(sensorActiveType());
  se["model"] = sensorModel();
  se["state"] = sensorStateText();
  se["age"] = sensorLastOkAgeS();
  se["reads"] = sensorReadCount();
  se["errors"] = sensorErrorCount();
  float v;
  if (sensorTemperature(v)) se["t"] = serialized(String(v, 1));
  if (sensorHumidity(v)) se["h"] = serialized(String(v, 1));
  if (sensorPressure(v)) se["p"] = serialized(String(v, 1));

  JsonObject ow = doc["owm"].to<JsonObject>();
  ow["on"] = cfg.owm.enabled;
  ow["state"] = weatherStateText();
  ow["err"] = weatherError();
  ow["place"] = weatherPlace();
  ow["age"] = weatherAgeS();
  ow["ok"] = weatherOkCount();
  ow["errors"] = weatherErrorCount();
  ow["req"] = weatherRequestCount();
  WeatherData wd;
  if (weatherData(wd)) {
    ow["t"] = serialized(String(wd.temp, 1));
    ow["feels"] = serialized(String(wd.feels, 1));
    ow["hum"] = wd.humidity;
    ow["press"] = wd.pressure;
    ow["wind"] = serialized(String(wd.wind, 1));
    ow["deg"] = wd.windDeg;
    ow["desc"] = wd.desc;
  }

  JsonObject d = doc["disp"].to<JsonObject>();
  d["item"] = displayItemName();
  d["mode"] = displayModeKey(displayMode());
  d["bright"] = displayBrightness();
  d["on"] = displayIsOn();
  d["hw"] = displayHwTypeActive();
  d["modules"] = displayModulesActive();

  JsonObject s = doc["sys"].to<JsonObject>();
  s["heap"] = ESP.getFreeHeap();
  s["heapMin"] = diagHeapMin();
  s["heapBoot"] = diagHeapBoot();
  s["maxBlock"] = heapMaxBlock();
  s["frag"] = heapFragmentation();
  s["uptime"] = diagUptimeS();
  s["loopMax"] = diagLoopMaxMs();
  s["reset"] = diagResetReason();
  s["cfgErr"] = configError;
  s["dirty"] = systemConfigDirty();
  s["restart"] = systemRestartNeeded();
  s["chip"] = String(chipId(), HEX);
  s["board"] = BOARD_NAME;
  s["flash"] = flashSize();
  s["sketch"] = sketchSize();
  s["sketchFree"] = sketchFreeSpace();
  s["otaMax"] = otaMaxSize();
  s["otaNet"] = otaNetworkEnabled();
  s["otaRssi"] = OTA_MIN_RSSI;
  uint32_t fsUsed, fsTotal;
  if (fsInfo(fsUsed, fsTotal)) {
    s["fsUsed"] = fsUsed;
    s["fsTotal"] = fsTotal;
  }
  sendJson(200, doc);
}

static void handleConfigGet() {
  if (!authorized()) return;
  JsonDocument doc;
  configToJson(cfg, doc.to<JsonObject>());
  sendJson(200, doc);
}

static void handleDefaults() {
  if (!authorized()) return;
  std::unique_ptr<Config> def(new (std::nothrow) Config);
  if (!def) return sendError(500, "nema memorije");
  configDefaults(*def);
  JsonDocument doc;
  configToJson(*def, doc.to<JsonObject>());
  // Wi-Fi i lozinke se ne vraćaju na podrazumevano preko ovog dugmeta
  doc["net"].remove("ssid");
  sendJson(200, doc);
}

static void handleConfigPost() {
  if (!authorized()) return;
  JsonDocument doc;
  if (!readBody(doc)) return;
  std::unique_ptr<Config> next(new (std::nothrow) Config(cfg));
  if (!next) return sendError(500, "nema memorije");
  char err[96];
  if (!configFromJson(*next, doc.as<JsonObjectConst>(), err, sizeof(err))) return sendError(400, err);
  bool save = server.arg("save") == "1";
  bool ok = systemApplyConfig(*next, save);
  if (!ok) return sendError(500, "podešavanja su primenjena, ali upis u flash nije uspeo");
  JsonDocument res;
  res["ok"] = true;
  res["saved"] = save;
  res["restart"] = systemRestartNeeded();
  res["restarting"] = systemRestartPending();
  sendJson(200, res);
}

static void handleMessage() {
  if (!authorized()) return;
  JsonDocument doc;
  if (!readBody(doc)) return;
  const char* text = doc["text"] | "";
  long seconds = doc["seconds"] | 30L;
  if (!text[0] || seconds <= 0) {
    displayPriorityClear();
    return sendOk();
  }
  if (strlen(text) > MSG_MAX_LEN) return sendError(400, "poruka je predugačka (najviše 150 bajtova)");
  if (seconds > 3600) return sendError(400, "trajanje najviše 3600 s");
  displayPriority(text, (uint16_t)seconds);
  sendOk();
}

static void handleAction() {
  if (!authorized()) return;
  JsonDocument doc;
  if (!readBody(doc)) return;
  const char* cmd = doc["cmd"] | "";
  if (!strcmp(cmd, "restart")) {
    systemRestart();
  } else if (!strcmp(cmd, "factory")) {
    systemFactoryReset();
  } else if (!strcmp(cmd, "test")) {
    displayTest();
  } else if (!strcmp(cmd, "portal")) {
    portalStart("zahtev iz web UI");
  } else if (!strcmp(cmd, "owm")) {
    weatherRefresh();
  } else if (!strcmp(cmd, "page")) {
    long page = doc["page"] | -1L;
    if (page < 0 || page >= PAGE_COUNT) return sendError(400, "nepoznata stranica");
    displayShowPage((uint8_t)page);
  } else if (!strcmp(cmd, "mode")) {
    DisplayMode m;
    if (!displayModeFromKey(doc["mode"] | "", m)) return sendError(400, "nepoznat režim prikaza");
    displaySetMode(m);
  } else {
    return sendError(400, "nepoznata komanda");
  }
  sendOk();
}

static void handleScan() {
  if (!authorized()) return;
  int n = WiFi.scanComplete();
  JsonDocument doc;
  if (n == WIFI_SCAN_RUNNING) {
    doc["scanning"] = true;
  } else if (n < 0 || server.arg("start") == "1") {
    WiFi.scanDelete();
    WiFi.scanNetworks(true);  // asinhrono
    doc["scanning"] = true;
  } else {
    JsonArray arr = doc["nets"].to<JsonArray>();
    for (int i = 0; i < n && i < 20; i++) {
      JsonObject o = arr.add<JsonObject>();
      o["ssid"] = WiFi.SSID(i);
      o["rssi"] = WiFi.RSSI(i);
      o["open"] = wifiScanIsOpen(i);
    }
    doc["scanning"] = false;
  }
  sendJson(200, doc);
}

static void handleRender() {
  if (!authorized()) return;
  long font = server.arg("font").toInt();
  if (font < 0 || font >= fontCount()) return sendError(400, "nepoznat font");
  const String& text = server.arg("text");
  if (text.length() > MAX_RENDER_TEXT) return sendError(400, "tekst za pregled je predugačak");
  char g[MAX_RENDER_TEXT + 1];
  utf8ToGlyphs(text.c_str(), g, sizeof(g));
  std::unique_ptr<uint8_t[]> cols(new (std::nothrow) uint8_t[MAX_RENDER_COLS]);
  if (!cols) return sendError(500, "nema memorije");
  uint16_t n = renderColumns((uint8_t)font, g, cols.get(), MAX_RENDER_COLS);
  JsonDocument doc;
  JsonArray arr = doc["cols"].to<JsonArray>();
  for (uint16_t i = 0; i < n; i++) arr.add(cols[i]);
  sendJson(200, doc);
}

static void handleMeta() {
  if (!authorized()) return;
  JsonDocument doc;
  JsonArray fonts = doc["fonts"].to<JsonArray>();
  for (uint8_t i = 0; i < fontCount(); i++) fonts.add(fontName(i));
  doc["msgMax"] = MSG_MAX_LEN;
  doc["msgCount"] = MSG_COUNT;
  doc["modMin"] = MODULES_MIN;
  doc["modMax"] = MODULES_MAX;
  // ploča: nazivi pinova i tastera za UI
  doc["board"] = BOARD_NAME;
  doc["button"] = BUTTON_NAME;
  doc["i2c"] = I2C_WIRING;
  JsonArray pins = doc["dhtPins"].to<JsonArray>();
  for (const PinOption& p : DHT_PINS) {
    JsonObject o = pins.add<JsonObject>();
    o["v"] = p.gpio;
    o["n"] = p.name;
  }
  sendJson(200, doc);
}

// Oznaka ploče u fajlu koji stiže (FW_TAG = "MCFW-TAG:<čip>;"). Svaki deo se pretražuje PRE
// upisa, pa se firmware za drugu ploču otkrije najkasnije u poslednjem delu, dok upis još nije
// završen — Update.end(false) ga tada odbacuje. (ESP8266 bi inače prihvatio ESP32 image: oba
// počinju bajtom 0xE9. ESP32 sam odbija image za drugi čip.) Fajl bez oznake (stariji firmware
// ovog projekta) se prihvata.
static uint8_t tagMatch = 0;  // poklopljenih znakova prefiksa "MCFW-TAG:"
static int8_t tagLen = -1;    // >= 0: hvata se naziv čipa
static bool tagDone = false;
static char tagChip[16];

static void tagReset() {
  tagMatch = 0;
  tagLen = -1;
  tagDone = false;
  tagChip[0] = '\0';
}

static void tagScan(const uint8_t* d, size_t n) {
  static const uint8_t prefixLen = (uint8_t)(strchr(FW_TAG, ':') - FW_TAG + 1);
  for (size_t i = 0; i < n && !tagDone; i++) {
    char c = (char)d[i];
    if (tagLen >= 0) {
      if (c == ';') {
        tagChip[tagLen] = '\0';
        tagDone = true;
      } else if (c < 0x20 || c > 0x7E || tagLen + 1 >= (int)sizeof(tagChip)) {
        tagLen = -1;  // nije oznaka, traži dalje
      } else {
        tagChip[tagLen++] = c;
      }
      continue;
    }
    if (c == FW_TAG[tagMatch]) {
      if (++tagMatch == prefixLen) {
        tagMatch = 0;
        tagLen = 0;
      }
    } else {
      tagMatch = c == FW_TAG[0] ? 1 : 0;
    }
  }
}

// delovi fajla stižu redom; Updater piše u slobodan prostor, a bootloader zamenjuje
// firmware tek posle uspešnog Update.end(). Prekid ostavlja postojeći firmware.
static void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START: {
      otaActive = otaDone = false;
      otaErr[0] = '\0';
      tagReset();
      if (cfg.adminPass[0] && !server.authenticate("admin", cfg.adminPass)) {
        strlcpySafe(otaErr, "potrebna je prijava", sizeof(otaErr));
        return;
      }
      size_t size = (size_t)server.arg("size").toInt();
      if (!otaPrecheck(size, otaErr, sizeof(otaErr))) {
        LOG("OTA (web): odbijeno — %s", otaErr);
        return;
      }
      LOG("OTA (web): početak, %s, %u B", up.filename.c_str(), (unsigned)size);
      mqttGoOffline();
      otaWebStart();
      displayHold("OTA");
      if (!Update.begin(size)) {
        snprintf_P(otaErr, sizeof(otaErr), PSTR("početak nije uspeo: %s"), updateError());
        displayResume();
        return;
      }
      otaActive = true;
      break;
    }
    case UPLOAD_FILE_WRITE:
      if (!otaActive) return;
      tagScan(up.buf, up.currentSize);
      if (tagDone && strcmp(tagChip, CHIP_NAME) != 0) {
        snprintf_P(otaErr, sizeof(otaErr), PSTR("firmware je za ploču %s, a ovaj sat je %s"), tagChip, CHIP_NAME);
        LOG("OTA (web): odbijeno — %s", otaErr);
        Update.end(false);  // upis nije završen: odbacuje se
        otaActive = false;
        displayResume();
        return;
      }
      if (Update.write(up.buf, up.currentSize) != up.currentSize) {
        snprintf_P(otaErr, sizeof(otaErr), PSTR("upis nije uspeo: %s"), updateError());
        Update.end(false);
        otaActive = false;
        displayResume();
        return;
      }
      otaWebProgress(Update.progress(), Update.size());
      break;
    case UPLOAD_FILE_END:
      if (!otaActive) return;
      otaActive = false;
      // end(false): veličina mora tačno da odgovara najavljenoj (zaštita od skraćenog fajla)
      if (Update.remaining()) {
        snprintf_P(otaErr, sizeof(otaErr), PSTR("fajl je kraći od najavljene veličine (nedostaje %u B)"),
                   (unsigned)Update.remaining());
        Update.end(false);
        displayResume();
      } else if (Update.end(false)) {
        otaDone = true;
        LOG("OTA (web): uspešno, %u B", (unsigned)up.totalSize);
        displayHold("OTA OK");
      } else {
        snprintf_P(otaErr, sizeof(otaErr), PSTR("provera nije uspela: %s"), updateError());
        displayResume();
      }
      break;
    case UPLOAD_FILE_ABORTED:
      if (otaActive) Update.end(false);
      otaActive = false;
      strlcpySafe(otaErr, "prenos je prekinut", sizeof(otaErr));
      displayResume();
      break;
  }
}

static void handleUpdateDone() {
  if (!authorized()) return;
  if (!otaDone) {
    LOG("OTA (web): neuspešno — %s; postojeći firmware ostaje", otaErr[0] ? otaErr : "nepoznata greška");
    return sendError(400, otaErr[0] ? otaErr : "ažuriranje nije uspelo");
  }
  sendOk();
  systemRestart(1000);
}

static void handleNotFound() {
  // captive portal: svaki nepoznat zahtev na pristupnoj tački vodi na podešavanje
  if (portalActive() && server.hostHeader() != portalIp()) {
    server.sendHeader(F("Location"), String(F("http://")) + portalIp() + "/", true);
    server.send(302, F("text/plain"), "");
    return;
  }
  server.send(404, F("text/plain"), F("Nije pronađeno"));
}

void webBegin() {
  server.on("/", HTTP_GET, handleIndex);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/defaults", HTTP_GET, handleDefaults);
  server.on("/api/message", HTTP_POST, handleMessage);
  server.on("/api/action", HTTP_POST, handleAction);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/render", HTTP_GET, handleRender);
  server.on("/api/meta", HTTP_GET, handleMeta);
  server.on("/api/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.onNotFound(handleNotFound);
  server.begin();
  LOG("Web: server pokrenut na portu 80");
}

void webLoop() { server.handleClient(); }
