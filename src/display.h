// Prikaz na matrici: rotacija stranica, poruke, obaveštenja, osvetljaj, test ekran.
// Sve je neblokirajuće; displayLoop() treba zvati što češće.
#pragma once
#include <Arduino.h>

void displayBegin();        // posle configLoad(); kreće sa niskim osvetljajem
void displayLoop();
void displayApplyConfig();  // posle promene podešavanja (tip modula traži restart)

// Sistemsko obaveštenje: prikazuje se jednom, na prvoj granici između stranica.
// translit=true: tekst je latinični srpski i prevodi se u ćirilicu ako je ona izabrana.
void displayNotice(const char* utf8, bool translit);
// Prioritetna poruka: prekida rotaciju i ponavlja se dok ne istekne.
void displayPriority(const char* utf8, uint16_t seconds);
void displayPriorityClear();
void displayTest();  // sve LED, pa brojevi modula

// Režim prikaza (ne čuva se; posle restarta je NORMAL).
// DND = "ne uznemiravaj": sat radi, poruke i obaveštenja se ne prikazuju.
enum DisplayMode : uint8_t { DM_NORMAL = 0, DM_OFF, DM_DND, DM_COUNT };
void displaySetMode(DisplayMode m);
DisplayMode displayMode();
const char* displayModeKey(DisplayMode m);  // "normal" / "off" / "dnd" (MQTT, API)
bool displayModeFromKey(const char* key, DisplayMode& out);

void displayShowPage(uint8_t page);         // odmah prikaži stranicu (PageId), pa nastavi rotaciju
void displaySetBrightness(uint8_t level);   // ručno, važi do sledeće smene dan/noć

uint8_t displayBrightness();  // trenutno primenjen nivo 0-15
bool displayIsOn();
const char* displayItemName();  // šta se trenutno prikazuje
uint8_t displayHwTypeActive();   // tip modula sa kojim je matrica pokrenuta
uint8_t displayModulesActive();  // broj modula sa kojim je matrica pokrenuta
