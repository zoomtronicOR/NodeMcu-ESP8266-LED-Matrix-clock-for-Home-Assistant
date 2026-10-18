// Spoljna temperatura preko OpenWeatherMap API-ja (opciono, cfg.owm).
// Zahtev je asinhron (ESPAsyncTCP, HTTP/1.0) — glavna petlja ne čeka server.
// HTTP, ne HTTPS: TLS na ESP8266 traži ~20 KB RAM-a, koliko uz ostale funkcije nema.
#pragma once
#include <Arduino.h>

void weatherBegin();
void weatherLoop();
void weatherApplyConfig();  // posle promene podešavanja: novo očitavanje odmah
void weatherRefresh();      // "Osveži sada"

struct WeatherData {
  float temp, feels, wind;  // °C, °C, m/s
  int16_t windDeg;          // pravac vetra 0-359° (odakle duva), -1 = nije poslat
  uint8_t humidity;         // %
  uint16_t pressure;        // hPa
  char desc[48];            // opis vremena (UTF-8, kako ga šalje server, lang=sr)
};

bool weatherTemperature(float& c);  // poslednja validna vrednost, ako nije prestara
bool weatherData(WeatherData& d);   // sve vrednosti, ako nisu prestare
const char* weatherStateText();
const char* weatherError();         // poslednja greška ("" = nema)
const char* weatherPlace();         // naziv mesta iz odgovora servera
int32_t weatherAgeS();              // starost poslednje validne vrednosti, -1 = nema
uint32_t weatherOkCount();
uint32_t weatherRequestCount();  // poslatih zahteva od pokretanja (najviše 1 u minuti)
uint32_t weatherErrorCount();
