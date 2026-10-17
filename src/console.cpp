#include "console.h"

#include <ESP8266WiFi.h>

#include <memory>

#include "app.h"
#include "config.h"
#include "diag.h"
#include "display.h"
#include "mqtt.h"
#include "net.h"
#include "sensors.h"
#include "system.h"
#include "timesync.h"

static char line[200];
static size_t len = 0;

static void printStatus() {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  Serial.printf_P(PSTR("Firmware %s, uptime %lu s, restart: %s\n"), FW_VERSION, (unsigned long)diagUptimeS(),
                  diagResetReason());
  Serial.printf_P(PSTR("Vreme: %02d:%02d:%02d %02d.%02d.%04d. (%s)\n"), t.tm_hour, t.tm_min, t.tm_sec, t.tm_mday,
                  t.tm_mon + 1, t.tm_year + 1900, timeStatusText(timeStatus()));
  Serial.printf_P(PSTR("Wi-Fi: %s, SSID '%s', IP %s, RSSI %d, neuspeha %lu, prekida %lu\n"), netStateText(),
                  cfg.wifiSsid, WiFi.localIP().toString().c_str(), netConnected() ? WiFi.RSSI() : 0,
                  (unsigned long)netFailCount(), (unsigned long)netDisconnectCount());
  Serial.printf_P(PSTR("Portal: %s (%s), MAC %s\n"), portalActive() ? "aktivan" : "ugašen", portalSsid(),
                  WiFi.macAddress().c_str());
  Serial.printf_P(PSTR("Heap: %u B (min %u, najveći blok %u, frag %u%%), najduža petlja %lu ms\n"),
                  ESP.getFreeHeap(), diagHeapMin(), ESP.getMaxFreeBlockSize(), ESP.getHeapFragmentation(),
                  (unsigned long)diagLoopMaxMs());
  Serial.printf_P(PSTR("Prikaz: %s, osvetljaj %u, %s\n"), displayItemName(), displayBrightness(),
                  displayIsOn() ? "uključen" : "isključen");
  Serial.printf_P(PSTR("MQTT: %s%s%s, povezivanja %lu, neuspeha %lu\n"), mqttStateText(),
                  mqttLastError()[0] ? " — " : "", mqttLastError(), (unsigned long)mqttConnectCount(),
                  (unsigned long)mqttFailCount());
  float sT, sH, sP;
  bool hasT = sensorTemperature(sT), hasH = sensorHumidity(sH), hasP = sensorPressure(sP);
  Serial.printf_P(PSTR("Senzor: %s %s — %s"), sensorTypeName(sensorActiveType()), sensorModel(), sensorStateText());
  if (hasT) Serial.printf_P(PSTR(", %.1f °C"), sT);
  if (hasH) Serial.printf_P(PSTR(", %.1f %%"), sH);
  if (hasP) Serial.printf_P(PSTR(", %.1f hPa"), sP);
  Serial.printf_P(PSTR(" (očitavanja %lu, grešaka %lu)\n"), (unsigned long)sensorReadCount(),
                  (unsigned long)sensorErrorCount());
  if (configError[0]) Serial.printf_P(PSTR("Greška konfiguracije: %s\n"), configError);
}

static void execute(char* cmd) {
  char* arg = strchr(cmd, ' ');
  if (arg) *arg++ = '\0';
  else arg = cmd + strlen(cmd);

  if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
    Serial.println(F("Komande:\n"
                     "  status                 stanje uređaja\n"
                     "  wifi <ssid>|<lozinka>  postavi i sačuvaj Wi-Fi mrežu\n"
                     "  portal                 pokreni pristupnu tačku za podešavanje\n"
                     "  msg <tekst>            prioritetna poruka 30 s\n"
                     "  test                   test ekran (sve LED + brojevi modula)\n"
                     "  restart                restart\n"
                     "  factory                fabrička podešavanja + restart"));
  } else if (!strcmp(cmd, "status")) {
    printStatus();
  } else if (!strcmp(cmd, "wifi")) {
    char* sep = strchr(arg, '|');
    const char* pass = "";
    if (sep) {
      *sep = '\0';
      pass = sep + 1;
    }
    std::unique_ptr<Config> next(new (std::nothrow) Config(cfg));
    if (!next) return;
    if (strlen(arg) >= sizeof(next->wifiSsid) || strlen(pass) >= sizeof(next->wifiPass)) {
      Serial.println(F("Greška: SSID ili lozinka su predugački"));
      return;
    }
    strlcpySafe(next->wifiSsid, arg, sizeof(next->wifiSsid));
    strlcpySafe(next->wifiPass, pass, sizeof(next->wifiPass));
    char err[96];
    if (!configValidate(*next, err, sizeof(err))) {
      Serial.printf_P(PSTR("Greška: %s\n"), err);
      return;
    }
    Serial.println(systemApplyConfig(*next, true) ? F("Sačuvano, povezujem se...") : F("Greška pri upisu u flash"));
  } else if (!strcmp(cmd, "portal")) {
    portalStart("serijska komanda");
  } else if (!strcmp(cmd, "msg")) {
    displayPriority(arg, 30);
  } else if (!strcmp(cmd, "test")) {
    displayTest();
  } else if (!strcmp(cmd, "restart")) {
    systemRestart(100);
  } else if (!strcmp(cmd, "factory")) {
    systemFactoryReset();
  } else if (cmd[0]) {
    Serial.printf_P(PSTR("Nepoznata komanda '%s' (help)\n"), cmd);
  }
}

void consoleLoop() {
  // čita samo ono što je stiglo; nikad ne čeka
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[len] = '\0';
      execute(line);
      len = 0;
    } else if (len + 1 < sizeof(line)) {
      line[len++] = c;
    }
  }
}
