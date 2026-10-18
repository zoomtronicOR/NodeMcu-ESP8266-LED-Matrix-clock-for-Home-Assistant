#include "weather.h"

#include <ArduinoJson.h>
#include <ESPAsyncTCP.h>

#include <new>

#include "app.h"
#include "config.h"
#include "net.h"

static const char OWM_HOST[] = "api.openweathermap.org";
constexpr size_t RESP_MAX = 2048;             // odgovor je obično ~500-800 B
constexpr uint32_t REQ_TIMEOUT_MS = 15000;
constexpr uint32_t RETRY_AFTER_ERROR_MS = 5UL * 60 * 1000;
// Tvrdi limit: najviše jedan zahtev u minuti, bez obzira na razlog (raspored, "Osveži sada",
// promena podešavanja, ponovni pokušaj). Raniji zahtev se odlaže, ne odbacuje.
constexpr uint32_t MIN_REQUEST_GAP_MS = 60UL * 1000;

enum WState : uint8_t { W_IDLE, W_BUSY };

static WState state = W_IDLE;
static AsyncClient* cli = nullptr;
static char* resp = nullptr;
static size_t respLen = 0;
// postavljaju asinhroni callback-ovi, obrađuje weatherLoop()
static volatile bool evDone = false, evFailed = false, evOverflow = false;
static char reqBuf[256];
static uint32_t reqT0 = 0, lastAttempt = 0;
static bool attempted = false, lastFailed = false, forceNow = false;
static bool everSent = false;  // ne briše se pri promeni podešavanja (limit od 1/min važi uvek)
static uint32_t reqCount = 0;

static WeatherData data;
static bool valid = false;
static uint32_t lastOkMs = 0;
static char err[64] = "";
static char place[33] = "";
static uint32_t okCount = 0, errCount = 0;

// ---------------------------------------------------------------- pomoćne

static void setError(const char* e) {
  strlcpySafe(err, e, sizeof(err));
  errCount++;
  lastFailed = true;
  LOG("OWM: %s", err);
}

static void urlEncode(const char* in, char* out, size_t size) {
  static const char HEX_DIGITS[] = "0123456789ABCDEF";
  size_t n = 0;
  for (const uint8_t* p = (const uint8_t*)in; *p && n + 4 < size; p++) {
    if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == ',') {
      out[n++] = (char)*p;
    } else {
      out[n++] = '%';
      out[n++] = HEX_DIGITS[*p >> 4];
      out[n++] = HEX_DIGITS[*p & 15];
    }
  }
  out[n] = '\0';
}

static void freeRequest() {
  if (cli) {
    cli->onConnect(nullptr, nullptr);
    cli->onData(nullptr, nullptr);
    cli->onDisconnect(nullptr, nullptr);
    cli->onError(nullptr, nullptr);
    if (cli->connected()) cli->close(true);
    delete cli;
    cli = nullptr;
  }
  delete[] resp;
  resp = nullptr;
  respLen = 0;
}

// ---------------------------------------------------------------- zahtev

static void startRequest() {
  attempted = true;
  everSent = true;
  lastAttempt = millis();
  forceNow = false;
  reqCount++;
  char city[3 * 48 + 1];
  urlEncode(cfg.owm.city, city, sizeof(city));
  // samo cifre = ID grada (id=), inače naziv (q=)
  bool isId = cfg.owm.city[0] != '\0';
  for (const char* p = cfg.owm.city; *p; p++)
    if (!isdigit((uint8_t)*p)) isId = false;
  int n = snprintf_P(reqBuf, sizeof(reqBuf),
                     PSTR("GET /data/2.5/weather?%s=%s&appid=%s&units=metric&lang=sr HTTP/1.0\r\nHost: %s\r\n"
                          "Connection: close\r\n\r\n"),
                     isId ? "id" : "q", city, cfg.owm.key, OWM_HOST);
  if (n <= 0 || (size_t)n >= sizeof(reqBuf)) return setError("predugačak zahtev (naziv grada)");

  resp = new (std::nothrow) char[RESP_MAX + 1];
  cli = new (std::nothrow) AsyncClient();
  if (!resp || !cli) {
    freeRequest();
    return setError("nema memorije za zahtev");
  }
  evDone = evFailed = evOverflow = false;
  cli->onConnect([](void*, AsyncClient* c) { c->write(reqBuf, strlen(reqBuf)); }, nullptr);
  cli->onData(
      [](void*, AsyncClient*, void* data, size_t len) {
        size_t room = RESP_MAX - respLen;
        if (len > room) {
          evOverflow = true;
          len = room;
        }
        memcpy(resp + respLen, data, len);
        respLen += len;
      },
      nullptr);
  cli->onDisconnect([](void*, AsyncClient*) { evDone = true; }, nullptr);
  cli->onError([](void*, AsyncClient*, int8_t) { evFailed = true; }, nullptr);
  if (!cli->connect(OWM_HOST, 80)) {  // DNS i TCP idu asinhrono
    freeRequest();
    return setError("povezivanje sa serverom nije uspelo");
  }
  state = W_BUSY;
  reqT0 = millis();
}

static void parseResponse() {
  resp[respLen] = '\0';
  if (evOverflow) return setError("odgovor servera je prevelik");
  int code = 0;
  if (sscanf(resp, "HTTP/%*d.%*d %d", &code) != 1) return setError("neispravan odgovor servera");
  const char* body = strstr(resp, "\r\n\r\n");
  if (code == 401) return setError("neispravan API ključ (novi ključ se aktivira do 2 h)");
  if (code == 404) return setError("grad nije pronađen");
  if (code == 429) return setError("prekoračen limit zahteva");
  if (code != 200 || !body) {
    char e[40];
    snprintf_P(e, sizeof(e), PSTR("greška servera (HTTP %d)"), code);
    return setError(e);
  }
  JsonDocument filter;
  filter["main"]["temp"] = true;
  filter["main"]["feels_like"] = true;
  filter["main"]["humidity"] = true;
  filter["main"]["pressure"] = true;
  filter["wind"]["speed"] = true;
  filter["weather"][0]["description"] = true;
  filter["name"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body + 4, DeserializationOption::Filter(filter))) return setError("neispravan JSON");
  JsonVariantConst t = doc["main"]["temp"];
  if (!t.is<float>()) return setError("u odgovoru nema temperature");
  float v = t.as<float>();
  if (v < -80 || v > 60) return setError("temperatura van opsega");
  data.temp = v;
  data.feels = doc["main"]["feels_like"] | v;
  data.humidity = (uint8_t)constrain(doc["main"]["humidity"] | 0, 0, 100);
  data.pressure = (uint16_t)constrain(doc["main"]["pressure"] | 0, 0, 2000);
  data.wind = doc["wind"]["speed"] | 0.0f;
  strlcpySafe(data.desc, doc["weather"][0]["description"] | "", sizeof(data.desc));
  valid = true;
  lastOkMs = millis();
  okCount++;
  lastFailed = false;
  err[0] = '\0';
  strlcpySafe(place, doc["name"] | "", sizeof(place));
  LOG("OWM: %s %.1f °C, %s", place, data.temp, data.desc);
}

// ---------------------------------------------------------------- javni API

void weatherBegin() {}

void weatherApplyConfig() {
  if (state == W_BUSY) freeRequest();
  state = W_IDLE;
  valid = false;
  err[0] = '\0';
  place[0] = '\0';
  attempted = false;  // novo podešavanje: očitavanje odmah
}

void weatherRefresh() { forceNow = true; }

void weatherLoop() {
  if (state == W_BUSY) {
    bool timeout = elapsed(reqT0, REQ_TIMEOUT_MS);
    if (!evDone && !evFailed && !timeout) return;
    if (timeout && !evDone) setError("server ne odgovara (isteklo vreme)");
    else if (respLen == 0) setError("veza sa serverom nije uspela");
    else parseResponse();
    freeRequest();
    state = W_IDLE;
    return;
  }
  if (!cfg.owm.enabled || !cfg.owm.key[0] || !cfg.owm.city[0] || !netConnected()) return;
  uint32_t interval = cfg.owm.intervalMin * 60000UL;
  uint32_t wait = lastFailed ? min(RETRY_AFTER_ERROR_MS, interval) : interval;
  bool due = forceNow || !attempted || elapsed(lastAttempt, wait);
  if (due && (!everSent || elapsed(lastAttempt, MIN_REQUEST_GAP_MS))) startRequest();
}

static bool fresh() {
  if (!cfg.owm.enabled || !valid) return false;
  // prestaro posle tri propuštena osvežavanja (a najmanje 1 h)
  uint32_t limit = max((uint32_t)3600000UL, (uint32_t)(cfg.owm.intervalMin * 3 * 60000UL));
  return !elapsed(lastOkMs, limit);
}

bool weatherTemperature(float& c) {
  if (!fresh()) return false;
  c = data.temp;
  return true;
}

bool weatherData(WeatherData& d) {
  if (!fresh()) return false;
  d = data;
  return true;
}

const char* weatherStateText() {
  if (!cfg.owm.enabled) return "isključeno";
  if (!cfg.owm.key[0] || !cfg.owm.city[0]) return "nije podešeno (ključ i grad)";
  if (state == W_BUSY) return "čeka odgovor servera";
  if ((forceNow || !attempted) && everSent && !elapsed(lastAttempt, MIN_REQUEST_GAP_MS))
    return "zahtev čeka (najviše 1 u minuti)";
  float t;
  if (weatherTemperature(t)) return lastFailed ? "radi (poslednje osvežavanje nije uspelo)" : "radi";
  if (!netConnected()) return "čeka Wi-Fi";
  return attempted ? "greška" : "čeka prvo očitavanje";
}

const char* weatherError() { return err; }
const char* weatherPlace() { return place; }
int32_t weatherAgeS() { return valid ? (int32_t)((millis() - lastOkMs) / 1000) : -1; }
uint32_t weatherOkCount() { return okCount; }
uint32_t weatherRequestCount() { return reqCount; }
uint32_t weatherErrorCount() { return errCount; }
