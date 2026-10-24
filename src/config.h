// Konfiguracija: struktura, podrazumevane vrednosti, validacija, čuvanje u LittleFS (JSON).
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

constexpr uint16_t CONFIG_SCHEMA = 4;  // povećati pri promeni strukture; starije se prevode pri učitavanju
constexpr uint8_t MSG_COUNT = 6;
constexpr uint8_t MSG_MAX_LEN = 150;   // bajtova UTF-8 po poruci
constexpr uint8_t PAGE_COUNT = 6;

enum PageId : uint8_t { PAGE_TIME = 0, PAGE_DATE, PAGE_WEEKDAY, PAGE_MESSAGES, PAGE_SENSOR, PAGE_WEATHER };
enum ScrollDir : uint8_t { SCROLL_LEFT = 0, SCROLL_RIGHT, SCROLL_UP, SCROLL_DOWN };
enum NightMode : uint8_t { NIGHT_DIM = 0, NIGHT_OFF };
enum AnimDir : uint8_t { ANIM_NONE = 0, ANIM_UP, ANIM_DOWN };
constexpr uint8_t DATE_FORMAT_COUNT = 6;
constexpr uint8_t HW_TYPE_COUNT = 12;  // MD_MAX72XX::moduleType_t
constexpr uint8_t COLON_STYLE_COUNT = 3;
constexpr uint8_t DATE_DOT_COUNT = 3;

enum MatrixType : uint8_t { MATRIX_MAX7219 = 0, MATRIX_WS2812 };
// raspored LED-ova WS2812B matrice (bitovi cfg.wsLayout)
enum WsLayout : uint8_t { WS_COLUMNS = 1, WS_SERPENTINE = 2, WS_START_RIGHT = 4, WS_START_BOTTOM = 8 };
constexpr uint8_t WS_MODULES_MAX = 8;  // 64 × 8 = 512 LED-ova (RAM i vreme slanja kadra)
// boje WS2812B: indeksi 0-5 = PageId, zatim sistemske poruke (obaveštenja, portal, OTA, test)
constexpr uint8_t COLOR_SYSTEM = PAGE_COUNT;
constexpr uint8_t COLOR_COUNT = PAGE_COUNT + 1;

struct Message {
  bool enabled;
  char text[MSG_MAX_LEN + 1];
};

struct MqttConfig {
  bool enabled;
  char host[65];
  uint16_t port;
  char user[33];
  char pass[65];
  char base[33];        // osnovna tema, npr. matrix_clock_69bf
  bool discovery;       // Home Assistant MQTT Discovery
  char discPrefix[25];  // obično "homeassistant"
  uint16_t teleSeconds; // interval telemetrije
};

struct SensorConfig {
  uint8_t type;       // SensorType (sensors.h)
  uint8_t dhtPin;     // GPIO za DHT22, jedan od DHT_PINS (board.h)
  uint16_t interval;  // s između očitavanja
  int16_t tOffset;    // kalibracioni pomak, desetinke °C
  int16_t hOffset;    // kalibracioni pomak, desetinke %
  bool showTemp, showHum, showPress;  // šta se prikazuje na stranici "Senzor"
  bool boldDigits;                    // stranica "Senzor" fontom sa masnim ciframa i jedinicama
};

struct WeatherConfig {  // OpenWeatherMap
  bool enabled;
  char key[65];       // API ključ (tajna, ne izvozi se)
  char city[49];      // npr. "Beograd,RS"
  uint16_t intervalMin;
  char label[33];     // natpis na matrici, UTF-8 (npr. "Napolju")
  bool showTemp, showFeels, showHum, showPress, showWind, showWindDir, showDesc;  // šta se prikazuje
  bool boldDigits;  // stranica "Napolju" fontom sa masnim ciframa i jedinicama
};

struct Config {
  // mreža
  char hostname[25];
  char wifiSsid[33];
  char wifiPass[65];
  char apPass[65];     // lozinka pristupne tačke za podešavanje (8-63 znaka)
  char adminPass[33];  // prazno = web UI bez lozinke
  uint8_t portalAfterFails;

  // vreme
  char tzName[33];
  char tz[49];  // POSIX TZ string
  char ntp[3][41];
  bool h24;
  bool showSeconds;
  uint8_t animDir;  // AnimDir: kotrljanje cifara koje se promene
  bool animHour, animMin, animSec;  // koje se cifre animiraju
  bool blinkColon;
  bool leadingZero;
  uint8_t dateFormat;
  uint8_t dateDot;  // tačke u datumu i temperaturi: 0 = 1, 1 = 2, 2 = 4 piksela
  bool dateNarrow;  // brojčani datum uskim ciframa (korisno na 3-4 modula)
  bool cyrillic;

  // prikaz
  uint8_t matrix;    // MatrixType (promena traži restart)
  uint8_t hwType;
  uint8_t modules;   // broj MAX7219 modula u lancu (MODULES_MIN..MODULES_MAX); WS2812B: širina / 8
  uint8_t wsLayout;  // WsLayout bitovi
  uint16_t wsMaxMa;  // WS2812B: najveća dozvoljena struja (procena), mA
  uint32_t colors[COLOR_COUNT];  // WS2812B: 0xRRGGBB po stranici + sistemske poruke
  uint8_t timeFont;  // indeks u FONT_TABLE (1..3)
  uint8_t colonStyle;  // dvotačka sata: 0 = 1+1, 1 = 2+2, 2 = 4+4 piksela
  uint8_t brightDay, brightNight, brightMax;  // 0..15
  bool nightEnabled;
  uint16_t nightStart, nightEnd;  // minuti od ponoći
  uint8_t nightMode;
  uint8_t scrollSpeed;   // ms po koraku
  uint16_t scrollPause;  // ms pauze na kraju poruke
  uint8_t scrollDir;
  uint8_t timeSeconds;   // trajanje stranice vremena u rotaciji
  uint8_t infoSeconds;   // trajanje statičkih stranica
  uint8_t pageOrder[PAGE_COUNT];
  bool pageEnabled[PAGE_COUNT];  // indeks = PageId

  Message msgs[MSG_COUNT];

  MqttConfig mqtt;
  SensorConfig sens;
  WeatherConfig owm;
};

extern Config cfg;
extern char configError[96];  // poslednja greška učitavanja (za dijagnostiku), prazno = OK

void configDefaults(Config& c);
bool configLoad();                 // false -> rade podrazumevane vrednosti, greška u configError
bool configSave(const Config& c);  // atomski upis (tmp + rename)
void configFactoryReset();         // briše fajl i vraća podrazumevano (Wi-Fi podaci se brišu)

// JSON; lozinke se nikad ne izvoze, samo indikator da su postavljene
void configToJson(const Config& c, JsonObject o);
// Delimično ažuriranje iz JSON-a + validacija. Vraća false i poruku ako nešto nije ispravno.
bool configFromJson(Config& c, JsonObjectConst o, char* err, size_t errLen);
bool configValidate(const Config& c, char* err, size_t errLen);
