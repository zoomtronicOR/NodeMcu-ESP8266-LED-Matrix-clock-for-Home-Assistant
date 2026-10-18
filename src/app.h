// Zajedničke definicije: verzija, pinovi, logovanje.
#pragma once
#include <Arduino.h>

#define FW_VERSION "3.1.0"
extern const char FW_BUILD[];  // vreme builda (src/build_info.cpp)

// MAX7219 (softverski SPI) — D5/D7/D6 ostavljaju D1/D2 slobodnim za I²C
constexpr uint8_t PIN_MAX_CLK = 14;  // D5
constexpr uint8_t PIN_MAX_DIN = 13;  // D7
constexpr uint8_t PIN_MAX_CS = 12;   // D6
// broj MAX7219 modula je u cfg.modules (MODULES_MIN..MODULES_MAX); 4 x 8x8 = 8x32
constexpr uint8_t MODULES_MIN = 3;  // 24 kolone: HH:MM uskim fontom; sekunde se tada izostavljaju
constexpr uint8_t MODULES_MAX = 16;

// FLASH taster na NodeMCU ploči (GPIO0, aktivan na 0). Dug pritisak = Wi-Fi podešavanje.
constexpr uint8_t PIN_BUTTON = 0;

// Logovanje na serijski port; format ide u flash (PSTR)
#define LOG(fmt, ...) Serial.printf_P(PSTR("[%7lu] " fmt "\n"), (unsigned long)(millis() / 1000UL), ##__VA_ARGS__)

// Bezbedno kopiranje C stringa (uvek terminira, skraćuje)
inline void strlcpySafe(char* dst, const char* src, size_t size) {
  if (!size) return;
  if (!src) src = "";
  strncpy(dst, src, size - 1);
  dst[size - 1] = '\0';
}

// Proteklo vreme otporno na prelivanje millis()
inline bool elapsed(uint32_t since, uint32_t ms) { return (uint32_t)(millis() - since) >= ms; }
