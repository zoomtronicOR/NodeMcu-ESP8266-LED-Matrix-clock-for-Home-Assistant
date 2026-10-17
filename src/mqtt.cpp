#include "mqtt.h"

#include <ArduinoJson.h>
#include <AsyncMqttClient.h>
#include <ESP8266WiFi.h>

#include "app.h"
#include "diag.h"
#include "display.h"
#include "net.h"
#include "sensors.h"
#include "system.h"
#include "timesync.h"

constexpr uint32_t BACKOFF_MIN_MS = 5000;
constexpr uint32_t BACKOFF_MAX_MS = 5UL * 60 * 1000;
constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t CLOSE_TIMEOUT_MS = 3000;
constexpr uint32_t MIN_HEAP_FOR_PUBLISH = 12000;  // ne gomilati pakete kad je memorija tesna
constexpr size_t MAX_INBOUND = MSG_MAX_LEN + 64;  // veće komande se odbacuju
constexpr uint8_t INBOUND_SLOTS = 3;

enum MqState : uint8_t { MQ_OFF, MQ_WAIT, MQ_CONNECTING, MQ_CONNECTED, MQ_CLOSING };

static AsyncMqttClient client;
static MqState st = MQ_OFF;
static uint32_t stT0 = 0;
static uint32_t waitMs = 0;
static uint32_t backoffMs = BACKOFF_MIN_MS;
static uint32_t connects = 0, fails = 0;
static time_t lastConnect = 0;
static char lastError[48] = "";

// teme i identitet (AsyncMqttClient čuva samo pokazivače — moraju biti trajni)
static char uid[24];
static char topicStatus[48];
static char topicState[48];
static char topicTele[48];
static char topicSensor[48];
static char topicCmd[48];     // <base>/command/
static char topicCmdSub[48];  // <base>/command/#

// događaji iz asinhronih callback-ova; obrađuju se u mqttLoop()
static volatile bool evConnected = false;
static volatile bool evDisconnected = false;
static volatile uint8_t evReason = 0;

struct Inbound {
  char cmd[16];
  char payload[MAX_INBOUND + 1];
};
static Inbound inq[INBOUND_SLOTS];
static volatile uint8_t inHead = 0, inTail = 0;
static uint32_t rejected = 0;

static uint8_t discStep = 0xFF;  // sledeći discovery entitet; 0xFF = gotovo
static uint32_t lastTele = 0;
static bool telePending = false;
static char lastState[96] = "";
static uint32_t lastStateCheck = 0;
static uint32_t lastSensorPub = 0;
static bool sensorPubbed = false;
static float pubT = 0, pubH = 0, pubP = 0;

static char buf[768];  // zajednički bafer za JSON payload
static char topicBuf[112];

// ---------------------------------------------------------------- pomoćne

bool mqttConfigChanged(const MqttConfig& a, const MqttConfig& b) {
  return a.enabled != b.enabled || strcmp(a.host, b.host) || a.port != b.port || strcmp(a.user, b.user) ||
         strcmp(a.pass, b.pass) || strcmp(a.base, b.base) || a.discovery != b.discovery ||
         strcmp(a.discPrefix, b.discPrefix) || a.teleSeconds != b.teleSeconds;
}

static const char* reasonText(uint8_t r) {
  switch ((AsyncMqttClientDisconnectReason)r) {
    case AsyncMqttClientDisconnectReason::TCP_DISCONNECTED: return "broker nedostupan ili veza prekinuta";
    case AsyncMqttClientDisconnectReason::MQTT_UNACCEPTABLE_PROTOCOL_VERSION: return "nepodržana verzija protokola";
    case AsyncMqttClientDisconnectReason::MQTT_IDENTIFIER_REJECTED: return "client ID odbijen";
    case AsyncMqttClientDisconnectReason::MQTT_SERVER_UNAVAILABLE: return "broker nije spreman";
    case AsyncMqttClientDisconnectReason::MQTT_MALFORMED_CREDENTIALS: return "neispravni podaci za prijavu";
    case AsyncMqttClientDisconnectReason::MQTT_NOT_AUTHORIZED: return "pogrešno korisničko ime ili lozinka";
    case AsyncMqttClientDisconnectReason::ESP8266_NOT_ENOUGH_SPACE: return "nema memorije";
    default: return "nepoznata greška";
  }
}

static bool canPublish() { return st == MQ_CONNECTED && ESP.getFreeHeap() >= MIN_HEAP_FOR_PUBLISH; }

static bool publishJson(const char* topic, JsonDocument& doc, bool retain) {
  size_t n = measureJson(doc);
  if (n >= sizeof(buf)) {
    LOG("MQTT: payload za %s je prevelik (%u B)", topic, (unsigned)n);
    return true;  // ne pokušavati ponovo
  }
  serializeJson(doc, buf, sizeof(buf));
  return client.publish(topic, retain ? 1 : 0, retain, buf, n) != 0;
}

// ---------------------------------------------------------------- callback-ovi (asinhroni kontekst)

static void onConnect(bool) { evConnected = true; }

static void onDisconnect(AsyncMqttClientDisconnectReason reason) {
  evReason = (uint8_t)reason;
  evDisconnected = true;
}

static void onMessage(char* topic, char* payload, AsyncMqttClientMessageProperties, size_t len, size_t index,
                      size_t total) {
  // samo kompletne, kratke komande; ništa sporo ovde
  size_t pl = strlen(topicCmd);
  if (index != 0 || len != total || strncmp(topic, topicCmd, pl) != 0) return;
  const char* cmd = topic + pl;
  uint8_t next = (inHead + 1) % INBOUND_SLOTS;
  if (len > MAX_INBOUND || strlen(cmd) >= sizeof(inq[0].cmd) || next == inTail) {
    rejected++;
    return;
  }
  Inbound& in = inq[inHead];
  strlcpySafe(in.cmd, cmd, sizeof(in.cmd));
  memcpy(in.payload, payload, len);
  in.payload[len] = '\0';
  inHead = next;
}

// ---------------------------------------------------------------- komande

static void handleCommand(const char* cmd, char* p) {
  // ukloni razmake/nove redove na krajevima
  while (*p == ' ' || *p == '\n' || *p == '\r') p++;
  for (size_t l = strlen(p); l && (p[l - 1] == ' ' || p[l - 1] == '\n' || p[l - 1] == '\r');) p[--l] = '\0';
  LOG("MQTT: komanda '%s' = '%s'", cmd, p);

  if (!strcmp(cmd, "brightness")) {
    char* end;
    float v = strtof(p, &end);
    if (end == p || v < 0 || v > 15) goto bad;
    displaySetBrightness((uint8_t)(v + 0.5f));
  } else if (!strcmp(cmd, "message")) {
    const char* text = p;
    long seconds = 60;
    JsonDocument doc;
    if (p[0] == '{') {
      if (deserializeJson(doc, p)) goto bad;
      text = doc["text"] | "";
      seconds = doc["seconds"] | 60L;
    }
    if (!text[0] || seconds <= 0) {
      displayPriorityClear();
      return;
    }
    if (strlen(text) > MSG_MAX_LEN || seconds > 3600) goto bad;
    displayPriority(text, (uint16_t)seconds);
  } else if (!strcmp(cmd, "display_mode")) {
    DisplayMode m;
    if (!displayModeFromKey(p, m)) goto bad;
    displaySetMode(m);
  } else if (!strcmp(cmd, "page")) {
    static const char* const PAGES[PAGE_COUNT] = {"time", "date", "weekday", "messages"};
    uint8_t i = 0;
    while (i < PAGE_COUNT && strcmp(p, PAGES[i])) i++;
    if (i == PAGE_COUNT) goto bad;
    displayShowPage(i);
  } else if (!strcmp(cmd, "test")) {
    displayTest();
  } else if (!strcmp(cmd, "restart")) {
    systemRestart(1500);
  } else {
    goto bad;
  }
  return;
bad:
  rejected++;
  LOG("MQTT: odbačena komanda '%s'", cmd);
}

// ---------------------------------------------------------------- Home Assistant Discovery

struct Entity {
  const char* component;
  const char* object;
};
static const Entity ENTITIES[] = {
    {"sensor", "rssi"},  {"sensor", "uptime"},     {"sensor", "heap"},  {"sensor", "ntp"},
    {"sensor", "ip"},    {"number", "brightness"}, {"select", "mode"},  {"select", "page"},
    {"text", "message"}, {"button", "test"},       {"button", "restart"},
    {"sensor", "temperature"}, {"sensor", "humidity"}, {"sensor", "pressure"},
};
constexpr uint8_t ENTITY_COUNT = sizeof(ENTITIES) / sizeof(ENTITIES[0]);

static void buildEntity(uint8_t i, JsonDocument& d) {
  const char* obj = ENTITIES[i].object;
  char id[48];
  snprintf_P(id, sizeof(id), PSTR("%s_%s"), uid, obj);
  d["~"] = cfg.mqtt.base;
  d["uniq_id"] = id;
  d["avty_t"] = "~/status";
  JsonObject dev = d["dev"].to<JsonObject>();
  dev["ids"].to<JsonArray>().add(uid);
  dev["name"] = cfg.hostname;
  char mdl[40];
  snprintf_P(mdl, sizeof(mdl), PSTR("ESP8266 matrični sat 8x%u"), displayModulesActive() * 8);
  dev["mdl"] = mdl;
  dev["mf"] = "DIY";
  dev["sw"] = FW_VERSION;
  if (netConnected()) dev["cu"] = String(F("http://")) + WiFi.localIP().toString() + "/";

  switch (i) {
    case 0:
      d["name"] = "Wi-Fi signal";
      d["stat_t"] = "~/telemetry";
      d["val_tpl"] = "{{ value_json.rssi }}";
      d["unit_of_meas"] = "dBm";
      d["dev_cla"] = "signal_strength";
      d["stat_cla"] = "measurement";
      d["ent_cat"] = "diagnostic";
      break;
    case 1:
      d["name"] = "Uptime";
      d["stat_t"] = "~/telemetry";
      d["val_tpl"] = "{{ value_json.uptime }}";
      d["unit_of_meas"] = "s";
      d["dev_cla"] = "duration";
      d["stat_cla"] = "total_increasing";
      d["ent_cat"] = "diagnostic";
      break;
    case 2:
      d["name"] = "Slobodna memorija";
      d["stat_t"] = "~/telemetry";
      d["val_tpl"] = "{{ value_json.heap }}";
      d["unit_of_meas"] = "B";
      d["stat_cla"] = "measurement";
      d["ic"] = "mdi:memory";
      d["ent_cat"] = "diagnostic";
      break;
    case 3:
      d["name"] = "NTP status";
      d["stat_t"] = "~/telemetry";
      d["val_tpl"] = "{{ value_json.ntp }}";
      d["ic"] = "mdi:clock-check-outline";
      d["ent_cat"] = "diagnostic";
      break;
    case 4:
      d["name"] = "IP adresa";
      d["stat_t"] = "~/telemetry";
      d["val_tpl"] = "{{ value_json.ip }}";
      d["ic"] = "mdi:ip-network";
      d["ent_cat"] = "diagnostic";
      break;
    case 5:
      d["name"] = "Osvetljaj";
      d["stat_t"] = "~/state";
      d["val_tpl"] = "{{ value_json.brightness }}";
      d["cmd_t"] = "~/command/brightness";
      d["min"] = 0;
      d["max"] = 15;
      d["step"] = 1;
      d["mode"] = "slider";
      d["ic"] = "mdi:brightness-6";
      break;
    case 6: {
      d["name"] = "Režim prikaza";
      d["stat_t"] = "~/state";
      d["val_tpl"] = "{{ value_json.mode }}";
      d["cmd_t"] = "~/command/display_mode";
      JsonArray ops = d["ops"].to<JsonArray>();
      for (uint8_t m = 0; m < DM_COUNT; m++) ops.add(displayModeKey((DisplayMode)m));
      d["ic"] = "mdi:television-ambient-light";
      break;
    }
    case 7: {
      d["name"] = "Prikaži stranicu";
      d["cmd_t"] = "~/command/page";
      d["opt"] = true;
      JsonArray ops = d["ops"].to<JsonArray>();
      ops.add("time");
      ops.add("date");
      ops.add("weekday");
      ops.add("messages");
      d["ic"] = "mdi:page-next-outline";
      break;
    }
    case 8:
      d["name"] = "Poruka";
      d["cmd_t"] = "~/command/message";
      d["max"] = MSG_MAX_LEN;
      d["ic"] = "mdi:message-text-outline";
      break;
    case 9:
      d["name"] = "Test ekran";
      d["cmd_t"] = "~/command/test";
      d["ent_cat"] = "diagnostic";
      d["ic"] = "mdi:led-on";
      break;
    case 10:
      d["name"] = "Restart";
      d["cmd_t"] = "~/command/restart";
      d["dev_cla"] = "restart";
      d["ent_cat"] = "config";
      break;
    case 11:
      d["name"] = "Temperatura";
      d["stat_t"] = "~/sensor";
      d["val_tpl"] = "{{ value_json.temperature }}";
      d["unit_of_meas"] = "°C";
      d["dev_cla"] = "temperature";
      d["stat_cla"] = "measurement";
      break;
    case 12:
      d["name"] = "Vlažnost";
      d["stat_t"] = "~/sensor";
      d["val_tpl"] = "{{ value_json.humidity }}";
      d["unit_of_meas"] = "%";
      d["dev_cla"] = "humidity";
      d["stat_cla"] = "measurement";
      break;
    case 13:
      d["name"] = "Pritisak";
      d["stat_t"] = "~/sensor";
      d["val_tpl"] = "{{ value_json.pressure }}";
      d["unit_of_meas"] = "hPa";
      d["dev_cla"] = "atmospheric_pressure";
      d["stat_cla"] = "measurement";
      break;
  }
}

// entiteti senzora postoje samo ako ih izabrani senzor podržava
static bool entityAvailable(uint8_t i) {
  switch (i) {
    case 11: return sensorActiveType() != SENSOR_NONE;
    case 12: return sensorHasHumidity();
    case 13: return sensorHasPressure();
    default: return true;
  }
}

// jedan entitet po pozivu; kad je Discovery isključen, briše ranije objavljene entitete
static void discoveryStep() {
  if (discStep >= ENTITY_COUNT || !canPublish()) return;
  const Entity& e = ENTITIES[discStep];
  snprintf_P(topicBuf, sizeof(topicBuf), PSTR("%s/%s/%s/%s/config"), cfg.mqtt.discPrefix, e.component, uid,
             e.object);
  bool ok;
  if (cfg.mqtt.discovery && entityAvailable(discStep)) {
    JsonDocument d;
    buildEntity(discStep, d);
    ok = publishJson(topicBuf, d, true);
  } else {
    ok = client.publish(topicBuf, 1, true, "", 0) != 0;
  }
  if (ok && ++discStep == ENTITY_COUNT)
    LOG("MQTT: Discovery %s", cfg.mqtt.discovery ? "objavljen" : "uklonjen");
}

// ---------------------------------------------------------------- stanje i telemetrija

static void publishState(bool force) {
  JsonDocument d;
  d["brightness"] = displayBrightness();
  d["mode"] = displayModeKey(displayMode());
  d["on"] = displayIsOn();
  char tmp[sizeof(lastState)];
  serializeJson(d, tmp, sizeof(tmp));
  if (!force && !strcmp(tmp, lastState)) return;
  if (!canPublish()) return;
  if (client.publish(topicState, 1, true, tmp, strlen(tmp))) strlcpySafe(lastState, tmp, sizeof(lastState));
}

// senzor: objava pri značajnoj promeni ili po intervalu telemetrije
static void publishSensor() {
  float t = 0, h = 0, p = 0;
  bool ht = sensorTemperature(t), hh = sensorHumidity(h), hp = sensorPressure(p);
  if (!ht && !hh && !hp) return;
  bool changed = !sensorPubbed || (ht && fabsf(t - pubT) >= 0.2f) || (hh && fabsf(h - pubH) >= 1.0f) ||
                 (hp && fabsf(p - pubP) >= 0.5f);
  if (!changed && !elapsed(lastSensorPub, cfg.mqtt.teleSeconds * 1000UL)) return;
  if (!canPublish()) return;
  JsonDocument d;
  if (ht) d["temperature"] = serialized(String(t, 1));
  if (hh) d["humidity"] = serialized(String(h, 1));
  if (hp) d["pressure"] = serialized(String(p, 1));
  if (publishJson(topicSensor, d, true)) {
    sensorPubbed = true;
    lastSensorPub = millis();
    pubT = t;
    pubH = h;
    pubP = p;
  }
}

static void publishTelemetry() {
  JsonDocument d;
  d["rssi"] = WiFi.RSSI();
  d["uptime"] = diagUptimeS();
  d["heap"] = ESP.getFreeHeap();
  d["heapMin"] = diagHeapMin();
  d["ntp"] = timeStatusText(timeStatus());
  d["ip"] = WiFi.localIP().toString();
  d["fw"] = FW_VERSION;
  d["rejected"] = rejected;
  d["sensor"] = sensorStateText();
  if (publishJson(topicTele, d, false)) telePending = false;
}

// ---------------------------------------------------------------- tok veze

static void enterWait(uint32_t ms) {
  st = MQ_WAIT;
  stT0 = millis();
  waitMs = ms;
}

static void onConnectedEvent() {
  st = MQ_CONNECTED;
  connects++;
  lastConnect = time(nullptr);
  backoffMs = BACKOFF_MIN_MS;
  lastError[0] = '\0';
  LOG("MQTT: povezan na %s:%u kao '%s'", cfg.mqtt.host, cfg.mqtt.port, uid);
  client.publish(topicStatus, 1, true, "online");
  client.subscribe(topicCmdSub, 0);
  discStep = 0;
  telePending = true;
  lastState[0] = '\0';
  sensorPubbed = false;
}

static void onDisconnectedEvent(uint8_t reason) {
  if (st == MQ_CLOSING) {  // planirano zatvaranje zbog novih podešavanja
    mqttApplyConfig();
    return;
  }
  if (st != MQ_CONNECTED && st != MQ_CONNECTING) return;
  strlcpySafe(lastError, reasonText(reason), sizeof(lastError));
  if (st == MQ_CONNECTED) {
    LOG("MQTT: veza prekinuta (%s)", lastError);
    enterWait(BACKOFF_MIN_MS);
  } else {
    fails++;
    LOG("MQTT: povezivanje neuspešno (%s), sledeći pokušaj za %lu s", lastError, (unsigned long)(backoffMs / 1000));
    enterWait(backoffMs);
    backoffMs = min(backoffMs * 2, BACKOFF_MAX_MS);
  }
}

void mqttApplyConfig() {
  if (client.connected() && st != MQ_CLOSING) {
    // uredno: offline, pa zatvaranje; nova podešavanja se primenjuju kad veza stvarno padne
    client.publish(topicStatus, 1, true, "offline");
    client.disconnect(false);
    st = MQ_CLOSING;
    stT0 = millis();
    return;
  }
  if (st == MQ_CONNECTING) client.disconnect(true);

  snprintf_P(uid, sizeof(uid), PSTR("matrixclock_%06x"), (unsigned)ESP.getChipId());
  snprintf_P(topicStatus, sizeof(topicStatus), PSTR("%s/status"), cfg.mqtt.base);
  snprintf_P(topicState, sizeof(topicState), PSTR("%s/state"), cfg.mqtt.base);
  snprintf_P(topicTele, sizeof(topicTele), PSTR("%s/telemetry"), cfg.mqtt.base);
  snprintf_P(topicSensor, sizeof(topicSensor), PSTR("%s/sensor"), cfg.mqtt.base);
  snprintf_P(topicCmd, sizeof(topicCmd), PSTR("%s/command/"), cfg.mqtt.base);
  snprintf_P(topicCmdSub, sizeof(topicCmdSub), PSTR("%s/command/#"), cfg.mqtt.base);

  client.setServer(cfg.mqtt.host, cfg.mqtt.port);
  client.setCredentials(cfg.mqtt.user, cfg.mqtt.pass);
  client.setClientId(uid);
  client.setKeepAlive(30);
  client.setCleanSession(true);
  client.setWill(topicStatus, 1, true, "offline");

  backoffMs = BACKOFF_MIN_MS;
  lastError[0] = '\0';
  discStep = 0xFF;
  if (cfg.mqtt.enabled) {
    enterWait(0);
    LOG("MQTT: uključen, broker %s:%u, tema '%s'", cfg.mqtt.host, cfg.mqtt.port, cfg.mqtt.base);
  } else {
    st = MQ_OFF;
  }
}

void mqttBegin() {
  client.onConnect(onConnect);
  client.onDisconnect(onDisconnect);
  client.onMessage(onMessage);
  mqttApplyConfig();
}

void mqttLoop() {
  if (evConnected) {
    evConnected = false;
    onConnectedEvent();
  }
  if (evDisconnected) {
    evDisconnected = false;
    onDisconnectedEvent(evReason);
  }

  switch (st) {
    case MQ_OFF:
      return;
    case MQ_WAIT:
      if (netConnected() && elapsed(stT0, waitMs)) {
        st = MQ_CONNECTING;
        stT0 = millis();
        client.connect();  // asinhrono: DNS + TCP + CONNACK u pozadini
      }
      return;
    case MQ_CONNECTING:
      if (elapsed(stT0, CONNECT_TIMEOUT_MS)) {
        fails++;
        strlcpySafe(lastError, "isteklo vreme za povezivanje", sizeof(lastError));
        LOG("MQTT: %s", lastError);
        enterWait(backoffMs);  // pre disconnect-a: događaj u stanju WAIT se ignoriše
        backoffMs = min(backoffMs * 2, BACKOFF_MAX_MS);
        client.disconnect(true);
      }
      return;
    case MQ_CLOSING:
      if (elapsed(stT0, CLOSE_TIMEOUT_MS)) {
        client.disconnect(true);
        st = MQ_OFF;
        mqttApplyConfig();
      }
      return;
    case MQ_CONNECTED:
      break;
  }

  // komande (najviše jedna po prolazu, da petlja ostane kratka)
  if (inTail != inHead) {
    Inbound& in = inq[inTail];
    handleCommand(in.cmd, in.payload);
    inTail = (inTail + 1) % INBOUND_SLOTS;
  }
  discoveryStep();
  if (elapsed(lastStateCheck, 1000)) {
    lastStateCheck = millis();
    publishState(false);
    publishSensor();
  }
  if (telePending || elapsed(lastTele, cfg.mqtt.teleSeconds * 1000UL)) {
    lastTele = millis();
    telePending = true;
    if (canPublish()) publishTelemetry();
  }
}

bool mqttConnected() { return st == MQ_CONNECTED; }

const char* mqttStateText() {
  switch (st) {
    case MQ_OFF: return "isključen";
    case MQ_WAIT: return netConnected() ? "čeka ponovni pokušaj" : "čeka Wi-Fi";
    case MQ_CONNECTING: return "povezivanje";
    case MQ_CONNECTED: return "povezan";
    case MQ_CLOSING: return "zatvaranje";
  }
  return "-";
}

const char* mqttLastError() { return lastError; }
uint32_t mqttConnectCount() { return connects; }
uint32_t mqttFailCount() { return fails; }
time_t mqttLastConnect() { return lastConnect; }
