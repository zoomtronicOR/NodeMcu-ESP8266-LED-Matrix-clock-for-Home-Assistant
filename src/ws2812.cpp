#include "ws2812.h"

#include <NeoPixelBus.h>

#include "app.h"
#include "config.h"

#if defined(ESP8266)
// UART1 (TX = GPIO2/D4): bitovi idu kroz FIFO, prekidi ostaju uključeni (Wi-Fi ne trpi).
// Show() se vraća kad ceo kadar uđe u FIFO: ≈ 30 µs po LED-u, za 256 LED-ova ≈ 7,7 ms.
// Asinhrona varijanta se ne koristi: zamenjuje UART prekid, pa konzola ne bi primala komande.
typedef NeoEsp8266Uart1Ws2812xMethod WsMethod;
#else
// RMT kanal 0: kadar se šalje u pozadini
typedef NeoEsp32Rmt0Ws2812xMethod WsMethod;
#endif

static NeoPixelBus<NeoGrbFeature, WsMethod>* strip = nullptr;
static uint16_t cols = 0;

// kadar koji treba prikazati i kadar koji je poslednji poslat
static uint8_t bits[WS_MODULES_MAX * 8], shownBits[WS_MODULES_MAX * 8];
static uint32_t color[WS_MODULES_MAX * 8], shownColor[WS_MODULES_MAX * 8];
static uint8_t level = 0, shownLevel = 0xFF;
static bool on = true, shownOn = true;
static uint8_t shownLayout = 0xFF;
static uint16_t shownMaxMa = 0;
static uint16_t lastMa = 0;

// nivo 0-15 -> svetlina 0-255, približno geometrijski (oko jednak korak na svakom nivou);
// nivo 0 je, kao na MAX7219, najslabiji koji se još vidi
static const uint8_t LEVEL_SCALE[16] PROGMEM = {2, 3, 4, 5, 7, 10, 14, 19, 27, 37, 51, 70, 97, 134, 185, 255};

// procena struje: ~20 mA po kanalu pri 255, ~1 mA po LED-u i kad ne svetli
constexpr uint32_t MA_PER_CHANNEL = 20;
constexpr uint16_t MA_IDLE_PER_LED = 1;

// redni broj LED-a za tačku (x = 0 levo, y = 0 gore)
static uint16_t ledIndex(uint16_t x, uint8_t y, uint8_t layout) {
  if (layout & WS_START_RIGHT) x = cols - 1 - x;
  if (layout & WS_START_BOTTOM) y = 7 - y;
  if (layout & WS_COLUMNS) {
    if ((layout & WS_SERPENTINE) && (x & 1)) y = 7 - y;
    return x * 8 + y;
  }
  if ((layout & WS_SERPENTINE) && (y & 1)) x = cols - 1 - x;
  return y * cols + x;
}

static uint8_t scaleChannel(uint8_t c, uint16_t scale) { return (uint8_t)((c * scale + 127) / 255); }

bool wsBegin(uint16_t columns) {
  cols = columns > sizeof(bits) ? sizeof(bits) : columns;
  strip = new NeoPixelBus<NeoGrbFeature, WsMethod>(cols * 8, PIN_WS_DATA);
  strip->Begin();
  strip->ClearTo(RgbColor(0));
  strip->Show();
  memset(bits, 0, sizeof(bits));
  memset(shownBits, 0, sizeof(shownBits));
  memset(color, 0, sizeof(color));
  memset(shownColor, 0, sizeof(shownColor));
  return true;
}

void wsSetColumn(uint16_t x, uint8_t b, uint32_t rgb) {
  if (x >= cols) return;
  bits[x] = b;
  color[x] = rgb;
}

void wsSetLevel(uint8_t l) { level = l > 15 ? 15 : l; }
void wsSetOn(bool o) { on = o; }
uint16_t wsCurrentMa() { return lastMa; }

void wsShow() {
  if (!strip) return;
  bool changed = level != shownLevel || on != shownOn || cfg.wsLayout != shownLayout || cfg.wsMaxMa != shownMaxMa ||
                 memcmp(bits, shownBits, cols) != 0 || memcmp(color, shownColor, cols * sizeof(color[0])) != 0;
  if (!changed) return;
  if (!strip->CanShow()) return;  // prethodni kadar se još šalje; ponovo u sledećem prolazu

  uint16_t idle = cols * 8 * MA_IDLE_PER_LED;
  uint16_t scale = on ? pgm_read_byte(&LEVEL_SCALE[level]) : 0;
  // prvi prolaz: struja pri izabranom nivou; ako prelazi ograničenje, svetlina se smanjuje
  uint32_t sum = 0;  // zbir vrednosti kanala svih upaljenih LED-ova
  for (uint16_t x = 0; x < cols && scale; x++) {
    uint32_t c = color[x];
    uint16_t ch = scaleChannel(c >> 16, scale) + scaleChannel((c >> 8) & 0xFF, scale) + scaleChannel(c & 0xFF, scale);
    sum += __builtin_popcount(bits[x]) * ch;
  }
  uint32_t ma = sum * MA_PER_CHANNEL / 255;
  uint32_t budget = cfg.wsMaxMa > idle + 50 ? cfg.wsMaxMa - idle : 50;
  if (ma > budget) {
    scale = (uint16_t)(scale * budget / ma);
    if (!scale) scale = 1;
    ma = budget;
  }

  uint8_t layout = cfg.wsLayout;
  for (uint16_t x = 0; x < cols; x++) {
    uint32_t c = color[x];
    RgbColor rgb(scaleChannel(c >> 16, scale), scaleChannel((c >> 8) & 0xFF, scale), scaleChannel(c & 0xFF, scale));
    // pri najnižoj svetlini tamna komponenta boje bi se zaokružila na 0: najjača ostaje bar 1
    if (scale && c && !rgb.R && !rgb.G && !rgb.B) {
      uint8_t r = c >> 16, g = (c >> 8) & 0xFF, b = c & 0xFF;
      if (r >= g && r >= b) rgb.R = 1;
      else if (g >= b) rgb.G = 1;
      else rgb.B = 1;
    }
    for (uint8_t y = 0; y < 8; y++)
      strip->SetPixelColor(ledIndex(x, y, layout), (bits[x] >> y) & 1 ? rgb : RgbColor(0));
  }
  strip->Show();
  lastMa = idle + ma;

  memcpy(shownBits, bits, cols);
  memcpy(shownColor, color, cols * sizeof(color[0]));
  shownLevel = level;
  shownOn = on;
  shownLayout = layout;
  shownMaxMa = cfg.wsMaxMa;
}
