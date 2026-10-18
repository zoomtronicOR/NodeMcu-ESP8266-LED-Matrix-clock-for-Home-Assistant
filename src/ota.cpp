#include "ota.h"

#include <ArduinoOTA.h>
#if defined(ESP8266)
#include <ESP8266mDNS.h>
#else
#include <ESPmDNS.h>
#endif

#include "app.h"
#include "config.h"
#include "display.h"
#include "mqtt.h"
#include "net.h"
#include "platform.h"

static bool netOta = false;
static uint8_t lastPct = 255;
static char activePass[sizeof(cfg.adminPass)] = "";  // lozinka sa kojom ArduinoOTA radi

#if defined(ESP8266)
size_t otaMaxSize() { return (sketchFreeSpace() - 0x1000) & 0xFFFFF000; }
#else
size_t otaMaxSize() { return ESP.getFreeSketchSpace(); }  // cela druga OTA particija
#endif

bool otaPrecheck(size_t size, char* err, size_t errLen) {
  err[0] = '\0';
  if (!netConnected()) {
    strlcpySafe(err, "nema Wi-Fi veze (ažuriranje preko pristupne tačke nije dozvoljeno)", errLen);
  } else if (WiFi.RSSI() < OTA_MIN_RSSI) {
    snprintf_P(err, errLen, PSTR("Wi-Fi signal je preslab (%d dBm, potrebno bar %d dBm)"), WiFi.RSSI(), OTA_MIN_RSSI);
  } else if (size == 0) {
    strlcpySafe(err, "nepoznata veličina fajla", errLen);
  } else if (size > otaMaxSize()) {
    snprintf_P(err, errLen, PSTR("firmware je prevelik (%u B, slobodno %u B)"), (unsigned)size, (unsigned)otaMaxSize());
  } else if (ESP.getFreeHeap() < OTA_MIN_HEAP) {
    strlcpySafe(err, "nema dovoljno slobodne memorije", errLen);
  }
  return err[0] == '\0';
}

static void showProgress(uint32_t done, uint32_t total) {
  uint8_t pct = total ? (uint8_t)(done * 100ULL / total) : 0;
  if (pct / 5 == lastPct / 5 && lastPct != 255) return;  // osveži na svakih 5 %
  lastPct = pct;
  char b[16];
  snprintf_P(b, sizeof(b), PSTR("OTA %u%%"), pct);
  displayHold(b);
}

void otaBegin() {
  strlcpySafe(activePass, cfg.adminPass, sizeof(activePass));
  // bez lozinke se ne otvara mrežni OTA (bilo ko na mreži bi mogao da upiše firmware)
  if (!cfg.adminPass[0]) {
    LOG("OTA: mrežni OTA (espota) je isključen dok se ne postavi admin lozinka");
    return;
  }
  ArduinoOTA.setHostname(cfg.hostname);
  ArduinoOTA.setPort(8266);  // isti port na obe ploče (ESP32 bi inače koristio 3232)
  ArduinoOTA.setPassword(cfg.adminPass);
  ArduinoOTA.onStart([]() {
    LOG("OTA: početak (espota)");
    lastPct = 255;
    mqttGoOffline();
    displayHold("OTA");
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) { showProgress(done, total); });
  ArduinoOTA.onEnd([]() {
    LOG("OTA: uspešno, restart");
    displayHold("OTA OK");
  });
  ArduinoOTA.onError([](ota_error_t e) {
    LOG("OTA: greška %u — postojeći firmware ostaje", (unsigned)e);
    displayResume();
  });
  // mDNS već vodi net.cpp; ovde samo dodajemo servis
#if defined(ESP8266)
  ArduinoOTA.begin(false);
#else
  ArduinoOTA.setMdnsEnabled(false);
  ArduinoOTA.begin();
#endif
  netOta = true;
  LOG("OTA: mrežni OTA aktivan na portu 8266");
}

void otaLoop() {
  // admin lozinka postavljena, promenjena ili uklonjena u radu: primeni bez restarta
  if (strcmp(activePass, cfg.adminPass) != 0) {
    if (!cfg.adminPass[0]) {
      if (netOta) ArduinoOTA.end();
      netOta = false;
      activePass[0] = '\0';
      LOG("OTA: mrežni OTA isključen (admin lozinka uklonjena)");
    } else if (!netOta) {
      otaBegin();
    } else {
      ArduinoOTA.setPassword(cfg.adminPass);
      strlcpySafe(activePass, cfg.adminPass, sizeof(activePass));
      LOG("OTA: lozinka za mrežni OTA promenjena");
    }
  }
  if (!netOta) return;
  static bool announced = false;
  if (!announced && netConnected()) {
    // oglašavanje za Arduino IDE / espota preko mDNS (posle povezivanja, kad mDNS radi)
    MDNS.enableArduino(8266, true);
    announced = true;
  }
  ArduinoOTA.handle();
}

bool otaNetworkEnabled() { return netOta; }

// web upload koristi isti prikaz napretka
void otaWebProgress(uint32_t done, uint32_t total) { showProgress(done, total); }
void otaWebStart() { lastPct = 255; }
