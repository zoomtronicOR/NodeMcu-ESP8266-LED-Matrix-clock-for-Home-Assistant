#include "ota.h"

#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>

#include "app.h"
#include "config.h"
#include "display.h"
#include "mqtt.h"
#include "net.h"

static bool netOta = false;
static uint8_t lastPct = 255;
static char activePass[sizeof(cfg.adminPass)] = "";  // lozinka sa kojom ArduinoOTA radi

size_t otaMaxSize() { return (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000; }

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
  ArduinoOTA.begin(false);  // mDNS već vodi net.cpp; ovde samo dodajemo servis
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
