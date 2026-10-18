// Razlike između ESP8266 i ESP32 Arduino core-a na jednom mestu.
// Ostali moduli koriste ove funkcije umesto ESP.* poziva koji postoje samo na jednom čipu.
#pragma once
#include <Arduino.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <interrupts.h>
#elif defined(ESP32)
#include <WiFi.h>
#endif

void platformSerialBegin(uint32_t baud);
bool platformFsBegin();  // LittleFS; na ESP32 formatira praznu particiju pri prvom pokretanju

uint32_t chipId();  // 24 bita (kraj MAC adrese) — za podrazumevana imena uređaja
uint32_t heapMaxBlock();
uint8_t heapFragmentation();  // %
uint32_t flashSize();
uint32_t sketchSize();
uint32_t sketchFreeSpace();  // slobodan prostor za firmware (ESP8266: iza trenutnog; ESP32: u app particiji)
bool fsInfo(uint32_t& used, uint32_t& total);
void resetReasonText(char* buf, size_t len);

void wifiSetHostname(const char* name);
bool wifiScanIsOpen(int i);  // mreža iz rezultata skeniranja nema lozinku

// Kratko isključivanje prekida (DHT22 protokol, ~5 ms)
#if defined(ESP8266)
using InterruptLock = esp8266::InterruptLock;
#else
class InterruptLock {
 public:
  InterruptLock() { portENTER_CRITICAL(&mux); }
  ~InterruptLock() { portEXIT_CRITICAL(&mux); }

 private:
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
};
#endif
