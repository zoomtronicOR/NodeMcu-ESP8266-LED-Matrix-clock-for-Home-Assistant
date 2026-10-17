// Matrični sat V3 — ESP8266 NodeMCU + 4x MAX7219 (8x32)
//
// Glavna petlja je neblokirajuća: svaki modul ima svoj *Loop() koji se brzo vraća.
// Prikaz vremena ne zavisi od Wi-Fi-ja, NTP-a ni web UI-ja.
#include <Arduino.h>
#include <LittleFS.h>

#include "app.h"
#include "config.h"
#include "console.h"
#include "diag.h"
#include "display.h"
#include "mqtt.h"
#include "net.h"
#include "system.h"
#include "timesync.h"
#include "web.h"

void setup() {
  Serial.begin(115200);
  Serial.println();
  diagBegin();

  if (!LittleFS.begin()) {
    LOG("GREŠKA: LittleFS nije dostupan, rade podrazumevane vrednosti");
    configDefaults(cfg);
    strlcpySafe(configError, "LittleFS nije dostupan", sizeof(configError));
  } else {
    configLoad();
  }

  displayBegin();  // prvo matrica, sa niskim osvetljajem
  timeBegin();
  netBegin();
  webBegin();
  mqttBegin();
  LOG("Spreman, slobodan heap %u B", ESP.getFreeHeap());
}

void loop() {
  uint32_t t0 = micros();
  displayLoop();
  netLoop();
  webLoop();
  mqttLoop();
  consoleLoop();
  systemLoop();
  diagLoop(micros() - t0);
}
