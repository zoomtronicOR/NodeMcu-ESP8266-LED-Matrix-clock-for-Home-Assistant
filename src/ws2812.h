// WS2812B matrica 8 × N u boji. Prikaz (display.cpp) i dalje crta preko MD_Parola u bafer
// MD_MAX72XX; ovaj modul posle svake promene prepisuje taj kadar na LED-ove, svaku kolonu u
// zadatoj boji. Raspored LED-ova (cfg.wsLayout) i ograničenje struje (cfg.wsMaxMa) važe odmah.
#pragma once
#include <Arduino.h>

bool wsBegin(uint16_t cols);                              // svi LED-ovi ugašeni
void wsSetColumn(uint16_t x, uint8_t bits, uint32_t rgb);  // x = 0 levo, bit0 = gornji red
void wsSetLevel(uint8_t level);                           // 0-15, isti nivoi kao MAX7219
void wsSetOn(bool on);
void wsShow();               // šalje kadar ako se nešto promenilo (kadar, boja, nivo, raspored)
uint16_t wsCurrentMa();      // procena struje poslednjeg poslatog kadra
