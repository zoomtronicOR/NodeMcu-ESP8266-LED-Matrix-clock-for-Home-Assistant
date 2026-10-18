// Raspored pinova i osobine ploče. Ploča se bira okruženjem u platformio.ini:
//   nodemcuv2 — ESP8266 NodeMCU (podrazumevano)
//   esp32c3   — ESP32-C3 SuperMini (4 MB flash, USB-C, BOOT taster na GPIO9)
#pragma once
#include <Arduino.h>

struct PinOption {
  uint8_t gpio;
  const char* name;  // kako piše na ploči
};

#if defined(ESP8266)

#define BOARD_NAME "ESP8266 NodeMCU"
#define CHIP_NAME "ESP8266"

// MAX7219 (softverski SPI) — D5/D7/D6 ostavljaju D1/D2 slobodnim za I²C
constexpr uint8_t PIN_MAX_CLK = 14;  // D5
constexpr uint8_t PIN_MAX_DIN = 13;  // D7
constexpr uint8_t PIN_MAX_CS = 12;   // D6

// I²C magistrala za senzore (fiksno, odvojeno od matrice)
constexpr uint8_t PIN_I2C_SDA = 4;  // D2
constexpr uint8_t PIN_I2C_SCL = 5;  // D1
#define I2C_WIRING "SDA → D2, SCL → D1"

// FLASH taster na NodeMCU ploči (GPIO0, aktivan na 0). Dug pritisak = Wi-Fi podešavanje.
constexpr uint8_t PIN_BUTTON = 0;
#define BUTTON_NAME "FLASH"

// DHT22: zaseban pin ili jedan od I²C pinova (aktivan je samo jedan senzor)
constexpr PinOption DHT_PINS[] = {{16, "D0 (GPIO16)"}, {5, "D1 (GPIO5)"}, {4, "D2 (GPIO4)"}};

#elif defined(CONFIG_IDF_TARGET_ESP32C3)

#define BOARD_NAME "ESP32-C3 SuperMini"
#define CHIP_NAME "ESP32-C3"

// MAX7219 na pinovima koji su na ploči označeni kao SPI (SCK=4, MOSI=6, SS=7);
// SPI je i ovde softverski, pa izbor pinova nije vezan za periferiju.
constexpr uint8_t PIN_MAX_CLK = 4;
constexpr uint8_t PIN_MAX_DIN = 6;
constexpr uint8_t PIN_MAX_CS = 7;

// I²C na GPIO1/GPIO0. GPIO8 (plava LED) i GPIO9 (BOOT) su "strapping" pinovi i
// ne koriste se, iako ih neke šeme označavaju kao SDA/SCL.
constexpr uint8_t PIN_I2C_SDA = 1;
constexpr uint8_t PIN_I2C_SCL = 0;
#define I2C_WIRING "SDA → GPIO1, SCL → GPIO0"

// BOOT taster (GPIO9, aktivan na 0). Dug pritisak posle pokretanja = Wi-Fi podešavanje.
constexpr uint8_t PIN_BUTTON = 9;
#define BUTTON_NAME "BOOT"

constexpr PinOption DHT_PINS[] = {{3, "GPIO3"}, {10, "GPIO10"}, {1, "GPIO1 (I²C SDA)"}, {0, "GPIO0 (I²C SCL)"}};

#else
#error "Nepodržana ploča: podržani su ESP8266 (NodeMCU) i ESP32-C3"
#endif

constexpr uint8_t DHT_PIN_COUNT = sizeof(DHT_PINS) / sizeof(DHT_PINS[0]);
constexpr uint8_t DHT_PIN_DEFAULT = DHT_PINS[0].gpio;

inline bool dhtPinAllowed(uint8_t gpio) {
  for (const PinOption& p : DHT_PINS)
    if (p.gpio == gpio) return true;
  return false;
}
