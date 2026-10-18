#include "net.h"

#include <DNSServer.h>
#if defined(ESP8266)
#include <ESP8266mDNS.h>
#else
#include <ESPmDNS.h>
#endif

#include "app.h"
#include "config.h"
#include "display.h"
#include "platform.h"

constexpr uint32_t CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t BACKOFF_MIN_MS = 5000;
constexpr uint32_t BACKOFF_MAX_MS = 5UL * 60 * 1000;
constexpr uint32_t PORTAL_MIN_MS = 2UL * 60 * 1000;   // portal radi najmanje ovoliko
constexpr uint32_t PORTAL_IDLE_MS = 60UL * 1000;      // ...i gasi se kad nema klijenata ovoliko
constexpr uint32_t PORTAL_DEFER_MAX_MS = 60UL * 1000;  // najduže odlaganje STA pokušaja zbog klijenta portala
constexpr uint32_t BUTTON_HOLD_MS = 3000;
static const IPAddress AP_IP(192, 168, 4, 1);

enum StaState : uint8_t { STA_IDLE, STA_CONNECTING, STA_CONNECTED, STA_WAIT };

static StaState sta = STA_IDLE;
static uint32_t staT0 = 0;
static uint32_t backoffMs = BACKOFF_MIN_MS;
static uint32_t connectedSince = 0;
static bool everConnected = false;
static uint8_t failsSinceConnect = 0;
static uint32_t failTotal = 0;
static uint32_t disconnects = 0;
static bool mdnsStarted = false;
static uint32_t deferSince = 0;  // 0 = pokušaj nije odložen

static bool portal = false;
static uint32_t portalT0 = 0;
static uint32_t lastClientMs = 0;
static DNSServer dns;
static char apSsid[24];

static uint32_t buttonDownMs = 0;
static bool buttonHandled = false;

// force=true: korisnik je upravo zadao mrežu — pokušaj odmah, i ako je telefon na portalu
static void startConnect(bool force = false) {
  if (!cfg.wifiSsid[0]) {
    sta = STA_IDLE;
    return;
  }
  // dok je telefon povezan na portal, STA pokušaji menjaju kanal i prekidaju ga —
  // automatske pokušaje odloži, ali najviše PORTAL_DEFER_MAX_MS
  if (!force && portal && WiFi.softAPgetStationNum() > 0) {
    if (!deferSince) deferSince = millis() | 1;
    if (!elapsed(deferSince, PORTAL_DEFER_MAX_MS)) {
      sta = STA_WAIT;
      staT0 = millis();
      return;
    }
  }
  deferSince = 0;
  LOG("Wi-Fi: povezivanje na '%s'", cfg.wifiSsid);
  WiFi.begin(cfg.wifiSsid, cfg.wifiPass);
  sta = STA_CONNECTING;
  staT0 = millis();
}

static void onConnected() {
  sta = STA_CONNECTED;
  connectedSince = millis();
  everConnected = true;
  failsSinceConnect = 0;
  backoffMs = BACKOFF_MIN_MS;
  String ip = WiFi.localIP().toString();
  LOG("Wi-Fi: povezan, IP %s, RSSI %d dBm, kanal %d", ip.c_str(), (int)WiFi.RSSI(), (int)WiFi.channel());
  if (!mdnsStarted && MDNS.begin(cfg.hostname)) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
    LOG("mDNS: http://%s.local", cfg.hostname);
  }
  char msg[40];
  snprintf_P(msg, sizeof(msg), PSTR("IP %s"), ip.c_str());
  displayNotice(msg, false);
}

static void onFailed(const char* why) {
  failTotal++;
  if (failsSinceConnect < 255) failsSinceConnect++;
  WiFi.disconnect();
  sta = STA_WAIT;
  staT0 = millis();
  LOG("Wi-Fi: neuspešno (%s), sledeći pokušaj za %lu s", why, (unsigned long)(backoffMs / 1000));
  if (!everConnected && !portal && failsSinceConnect >= cfg.portalAfterFails)
    portalStart("Wi-Fi nije dostupan");
}

void portalStart(const char* reason) {
  if (portal) return;
  LOG("Portal: pokrećem pristupnu tačku '%s' (%s)", apSsid, reason);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(apSsid, cfg.apPass);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", AP_IP);
  portal = true;
  portalT0 = lastClientMs = millis();
}

static void portalStop() {
  LOG("Portal: gasim pristupnu tačku");
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  portal = false;
}

void netBegin() {
  snprintf_P(apSsid, sizeof(apSsid), PSTR("MatrixClock-%04X"), (unsigned)(chipId() & 0xFFFF));
  WiFi.persistent(false);  // podaci su u našoj konfiguraciji; ne trošiti flash SDK-a
  WiFi.setAutoReconnect(false);  // ponovno povezivanje vodi ovaj modul (rastući interval)
#if defined(ESP32)
  wifiSetHostname(cfg.hostname);  // ESP32: ime mora biti zadato pre pokretanja Wi-Fi-ja
#endif
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();  // prekini eventualni auto-connect sa podacima starog firmware-a
#if defined(MATRIX_WIFI_TX_POWER)
  // neke serije ESP32-C3 SuperMini ploča imaju lošu antenu i ne povezuju se pri punoj snazi
  WiFi.setTxPower(MATRIX_WIFI_TX_POWER);
#endif
  wifiSetHostname(cfg.hostname);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  if (!cfg.wifiSsid[0]) portalStart("mreža nije podešena");
  else startConnect();
}

void netReconnectNow() {
  WiFi.disconnect();
  failsSinceConnect = 0;
  backoffMs = BACKOFF_MIN_MS;
  wifiSetHostname(cfg.hostname);
  startConnect(true);
}

static void handleButton() {
  if (digitalRead(PIN_BUTTON) == LOW) {
    if (!buttonDownMs) buttonDownMs = millis() | 1;
    if (!buttonHandled && elapsed(buttonDownMs, BUTTON_HOLD_MS)) {
      buttonHandled = true;
      portalStart(BUTTON_NAME " taster");
    }
  } else {
    buttonDownMs = 0;
    buttonHandled = false;
  }
}

void netLoop() {
  static uint32_t lastCheck = 0;
  if (portal) dns.processNextRequest();
#if defined(ESP8266)
  if (mdnsStarted) MDNS.update();  // ESP32 mDNS radi u svom zadatku
#endif
  if (!elapsed(lastCheck, 100)) return;
  lastCheck = millis();

  handleButton();

  wl_status_t st = WiFi.status();
  switch (sta) {
    case STA_CONNECTING:
      if (st == WL_CONNECTED) onConnected();
#if defined(ESP8266)
      else if (st == WL_WRONG_PASSWORD) onFailed("pogrešna lozinka");
#else
      else if (st == WL_CONNECT_FAILED) onFailed("pogrešna lozinka ili odbijena veza");
#endif
      else if (elapsed(staT0, CONNECT_TIMEOUT_MS)) onFailed(st == WL_NO_SSID_AVAIL ? "mreža nije pronađena" : "isteklo vreme");
      break;
    case STA_CONNECTED:
      if (st != WL_CONNECTED) {
        disconnects++;
        LOG("Wi-Fi: veza prekinuta (status %d)", st);
        WiFi.disconnect();
        backoffMs = BACKOFF_MIN_MS;
        sta = STA_WAIT;
        staT0 = millis();
      }
      break;
    case STA_WAIT:
      if (elapsed(staT0, backoffMs)) {
        backoffMs = min(backoffMs * 2, BACKOFF_MAX_MS);
        startConnect();
      }
      break;
    case STA_IDLE:
      break;
  }

  if (portal) {
    if (WiFi.softAPgetStationNum() > 0) lastClientMs = millis();
    if (sta == STA_CONNECTED && elapsed(portalT0, PORTAL_MIN_MS) && elapsed(lastClientMs, PORTAL_IDLE_MS))
      portalStop();
  }
}

bool netConnected() { return sta == STA_CONNECTED; }
uint32_t netConnectedSince() { return connectedSince; }
uint32_t netFailCount() { return failTotal; }
uint32_t netDisconnectCount() { return disconnects; }

const char* netStateText() {
  switch (sta) {
    case STA_CONNECTED: return "povezan";
    case STA_CONNECTING: return "povezivanje";
    case STA_WAIT: return "čeka ponovni pokušaj";
    default: return "nije podešen";
  }
}

bool portalActive() { return portal; }
const char* portalSsid() { return apSsid; }
const char* portalIp() { return "192.168.4.1"; }
