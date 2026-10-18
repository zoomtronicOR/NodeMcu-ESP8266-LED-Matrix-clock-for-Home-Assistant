// Matrični sat V3 — ESP8266 NodeMCU ili ESP32-C3 SuperMini + 3-16 MAX7219 modula
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
#include "ota.h"
#include "platform.h"
#include "sensors.h"
#include "system.h"
#include "timesync.h"
#include "weather.h"
#include "web.h"

void setup() {
  platformSerialBegin(115200);
  Serial.println();
  diagBegin();

  if (!platformFsBegin()) {
    LOG("GREŠKA: LittleFS nije dostupan, rade podrazumevane vrednosti");
    configDefaults(cfg);
    strlcpySafe(configError, "LittleFS nije dostupan", sizeof(configError));
  } else {
    configLoad();
  }

  displayBegin();  // prvo matrica, sa niskim osvetljajem
  sensorBegin();
  timeBegin();
  netBegin();
  webBegin();
  mqttBegin();
  otaBegin();
  weatherBegin();
  LOG("Spreman, slobodan heap %u B", (unsigned)ESP.getFreeHeap());
}

void loop() {
  uint32_t t0 = micros();
  displayLoop();
  netLoop();
  otaLoop();
  webLoop();
  mqttLoop();
  sensorLoop();
  weatherLoop();
  consoleLoop();
  systemLoop();
  diagLoop(micros() - t0);
}
