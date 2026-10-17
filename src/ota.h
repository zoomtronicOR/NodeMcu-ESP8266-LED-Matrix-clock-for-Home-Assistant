// Ažuriranje firmware-a preko mreže.
//  - web UI: POST /api/update (multipart, ?size=N) — u web.cpp, provere ovde
//  - ArduinoOTA (espota, "pio run -e ota -t upload"): samo kad je postavljena admin lozinka
// Neuspelo ili prekinuto ažuriranje ostavlja postojeći firmware (Updater upisuje u slobodan
// prostor, a zamenu radi bootloader tek posle uspešnog kraja). Automatskog vraćanja na stari
// firmware nema: ako novi firmware ne radi, oporavak je preko USB/serijskog porta.
#pragma once
#include <Arduino.h>

constexpr int8_t OTA_MIN_RSSI = -80;           // slabiji signal = ne započinjati
constexpr uint32_t OTA_MIN_HEAP = 12000;

void otaBegin();
void otaLoop();

// Provera pre početka: Wi-Fi veza i signal, slobodan prostor, memorija. false + poruka.
bool otaPrecheck(size_t size, char* err, size_t errLen);
size_t otaMaxSize();       // najveći firmware koji staje u slobodan prostor
bool otaNetworkEnabled();  // ArduinoOTA sluša

// za web upload (web.cpp): isti prikaz napretka na matrici
void otaWebStart();
void otaWebProgress(uint32_t done, uint32_t total);
