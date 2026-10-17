#include "system.h"

#include "app.h"
#include "display.h"
#include "mqtt.h"
#include "net.h"
#include "timesync.h"

static uint32_t restartAt = 0;
static bool restartPending = false;
static bool dirty = false;
static char bootHostname[sizeof(cfg.hostname)];

void systemRestart(uint32_t delayMs) {
  LOG("Restart za %lu ms", (unsigned long)delayMs);
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
  return ok;
}

bool systemConfigDirty() { return dirty; }

bool systemRestartNeeded() {
  return cfg.hwType != displayHwTypeActive() || cfg.modules != displayModulesActive() ||
         (bootHostname[0] && strcmp(bootHostname, cfg.hostname) != 0);
}
