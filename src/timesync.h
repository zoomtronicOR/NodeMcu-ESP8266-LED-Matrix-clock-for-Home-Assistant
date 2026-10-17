// Vreme: NTP (SNTP iz lwIP-a radi u pozadini, neblokirajuće), vremenska zona, status.
// Između sinhronizacija i bez mreže vreme nastavlja iz internog sata.
#pragma once
#include <Arduino.h>
#include <time.h>

enum TimeStatus : uint8_t { TIME_WAITING = 0, TIME_SYNCED, TIME_UNAVAILABLE };

void timeBegin();        // primenjuje cfg.tz i NTP servere
void timeApplyConfig();  // posle promene podešavanja

bool timeValid();  // sistemsko vreme je postavljeno (makar jednom sinhronizovano)
TimeStatus timeStatus();
const char* timeStatusText(TimeStatus s);
time_t timeLastSync();  // 0 = nikad
uint32_t timeSyncCount();
