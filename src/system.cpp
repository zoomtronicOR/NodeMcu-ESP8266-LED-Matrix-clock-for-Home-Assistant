#include "system.h"

#include "app.h"
#include "display.h"
#include "mqtt.h"
#include "net.h"
#include "sensors.h"
#include "timesync.h"
#include "weather.h"

static uint32_t restartAt = 0;
static bool restartPending = false;
static bool dirty = false;
static char bootHostname[sizeof(cfg.hostname)];

void systemRestart(uint32_t delayMs) {
  LOG("Restart za %lu ms", (unsigned long)delayMs);
  mqttGoOffline();
  restartAt = millis() + delayMs;
  restartPending = true;
}

void systemFactoryReset() {
  configFactoryReset();
  systemRestart();
}

void systemLoop() {
  if (!bootHostname[0]) strlcpySafe(bootHostname, cfg.hostname, sizeof(bootHostname));
  if (restartPending && (int32_t)(millis() - restartAt) >= 0) {
    Serial.flush();
    ESP.restart();
  }
}

bool systemApplyConfig(const Config& next, bool save) {
  bool wifiChanged = strcmp(next.wifiSsid, cfg.wifiSsid) != 0 || strcmp(next.wifiPass, cfg.wifiPass) != 0;
  bool timeChanged = strcmp(next.tz, cfg.tz) != 0 || memcmp(next.ntp, cfg.ntp, sizeof(cfg.ntp)) != 0;
  // ime uređaja ulazi u Discovery (naziv uređaja u Home Assistantu)
  bool mqttChanged = mqttConfigChanged(next.mqtt, cfg.mqtt) || strcmp(next.hostname, cfg.hostname) != 0;
  bool owmChanged = next.owm.enabled != cfg.owm.enabled || strcmp(next.owm.key, cfg.owm.key) ||
                    strcmp(next.owm.city, cfg.owm.city);
  bool sensorChanged = next.sens.type != cfg.sens.type ||
                       (next.sens.type == SENSOR_DHT22 && next.sens.dhtPin != cfg.sens.dhtPin);
  bool matrixChanged = next.matrix != displayMatrixActive();
  cfg = next;
  bool ok = true;
  if (save) {
    ok = configSave(cfg);
    dirty = !ok;
  } else {
    dirty = true;
  }
  if (timeChanged) timeApplyConfig();
  displayApplyConfig();
  if (wifiChanged) netReconnectNow();
  if (mqttChanged) mqttApplyConfig();
  if (owmChanged) weatherApplyConfig();
  // drugi senzor = drugi pinovi/magistrala: najpouzdanije je čisto pokretanje
  if (save && ok && sensorChanged) {
    LOG("Senzor promenjen u '%s' — restart", sensorTypeName(cfg.sens.type));
    systemRestart(1500);
  }
  // druga vrsta matrice = drugi drajver i pinovi; bez restarta matrica ne bi ništa prikazivala
  if (save && ok && matrixChanged) {
    LOG("Vrsta matrice promenjena — restart");
    systemRestart(1500);
  }
  return ok;
}

bool systemConfigDirty() { return dirty; }

bool systemRestartPending() { return restartPending; }

bool systemRestartNeeded() {
  if (cfg.sens.type != sensorActiveType() || (cfg.sens.type == SENSOR_DHT22 && cfg.sens.dhtPin != sensorActivePin()))
    return true;
  // tip modula (orijentacija) važi samo za MAX7219
  return cfg.matrix != displayMatrixActive() ||
         (cfg.matrix == MATRIX_MAX7219 && cfg.hwType != displayHwTypeActive()) ||
         cfg.modules != displayModulesActive() ||
         (bootHostname[0] && strcmp(bootHostname, cfg.hostname) != 0);
}
