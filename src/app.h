// Zajedničke definicije: verzija, pinovi, logovanje.
#pragma once
#include <Arduino.h>

#include "board.h"  // pinovi i osobine izabrane ploče

#define FW_VERSION "3.1.0"
extern const char FW_BUILD[];  // vreme builda (src/build_info.cpp)
extern const char FW_TAG[];    // "MCFW-TAG:<čip>;" — oznaka ploče za proveru pri OTA

// broj MAX7219 modula je u cfg.modules (MODULES_MIN..MODULES_MAX); 4 x 8x8 = 8x32
constexpr uint8_t MODULES_MIN = 3;  // 24 kolone: HH:MM uskim fontom; sekunde se tada izostavljaju
constexpr uint8_t MODULES_MAX = 16;

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
