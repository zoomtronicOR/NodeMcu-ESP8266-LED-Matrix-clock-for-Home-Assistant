#include "display.h"

#include <MD_MAX72xx.h>
#include <MD_Parola.h>
#include <sys/time.h>

#include "app.h"
#include "config.h"
#include "net.h"
#include "sensors.h"
#include "text.h"
#include "timesync.h"

constexpr uint32_t STARTUP_DARK_MS = 1500;         // prvo nizak osvetljaj, zatim sačuvani
constexpr uint32_t PORTAL_INFO_EVERY_MS = 20000;   // koliko često ponoviti uputstvo za portal
constexpr uint32_t NTP_WARN_EVERY_MS = 3600UL * 1000;

enum Item : uint8_t {
  IT_NONE, IT_TIME, IT_DATE, IT_WEEKDAY, IT_MESSAGE, IT_SENSOR, IT_NOTICE, IT_PRIORITY, IT_PORTAL, IT_TEST
};

static MD_Parola* P = nullptr;
static uint8_t hwActive = 0;
static uint8_t modulesActive = 4;
static uint16_t displayCols = 32;  // modulesActive * 8

static Item item = IT_NONE;
static uint32_t itemT0 = 0;
static char glyphs[200];     // tekst koji se trenutno prikazuje (kodovi glifova)
static char utf8[200];       // radni bafer za sastavljanje teksta
static char timeGlyphs[12];  // poslednje prikazano vreme

static uint8_t pageCursor = 0;
static bool inMessages = false;
static uint8_t msgCursor = 0;
static bool inSensor = false;
static uint8_t sensorCursor = 0;

static char notice[96];
static bool noticePending = false;
static char prio[MSG_MAX_LEN + 1];
static bool prioActive = false;
static uint32_t prioT0 = 0, prioMs = 0;
static uint32_t lastPortalInfo = 0;
static uint32_t lastNtpWarn = 0;
static TimeStatus prevTimeStatus = TIME_WAITING;

static uint8_t brightness = 0;
static bool isOn = true;
static DisplayMode mode = DM_NORMAL;
static int8_t brightOverride = -1;  // ručni nivo do sledeće smene dan/noć
static int8_t lastNight = -1;
static int8_t forcedPage = -1;
static uint32_t bootMs = 0;
static bool startupDone = false;

static uint8_t testPhase = 0;
static uint32_t testT0 = 0;

// ---------------------------------------------------------------- pomoćne

static void appendSys(char* dst, size_t size, const char* latin) {
  // dodaje sistemski (latinični) tekst, preveden u ćirilicu ako je izabrana
  size_t n = strlen(dst);
  if (cfg.cyrillic) latinToCyrillic(latin, dst + n, size - n);
  else strlcpySafe(dst + n, latin, size - n);
}

static void appendRaw(char* dst, size_t size, const char* s) {
  size_t n = strlen(dst);
  strlcpySafe(dst + n, s, size - n);
}

// Prikazuje sadržaj bafera glyphs. Kratak tekst je statičan (holdMs), dugačak se skroluje.
static void showGlyphs(uint8_t font, uint16_t holdMs) {
  P->setFont(fontData(font));
  P->setCharSpacing(1);
  uint16_t w = P->getTextColumns(glyphs);
  uint8_t dir = cfg.scrollDir;
  if (w <= displayCols) {
    textEffect_t in = PA_PRINT, out = PA_NO_EFFECT;
    if (dir == SCROLL_UP) in = out = PA_SCROLL_UP;
    else if (dir == SCROLL_DOWN) in = out = PA_SCROLL_DOWN;
    P->displayText(glyphs, PA_CENTER, cfg.scrollSpeed, holdMs, in, out);
  } else {
    // gore/dole nema smisla za tekst duži od ekrana -> levo
    bool right = dir == SCROLL_RIGHT;
    textEffect_t e = right ? PA_SCROLL_RIGHT : PA_SCROLL_LEFT;
    P->displayText(glyphs, right ? PA_RIGHT : PA_LEFT, cfg.scrollSpeed, cfg.scrollPause, e, e);
  }
}

static void startUtf8(Item it, const char* text, uint8_t font, uint16_t holdMs) {
  item = it;
  itemT0 = millis();
  utf8ToGlyphs(text, glyphs, sizeof(glyphs));
  if (!glyphs[0]) strlcpySafe(glyphs, " ", sizeof(glyphs));  // prazan tekst: kontrolisano
  showGlyphs(font, holdMs);
}

// ---------------------------------------------------------------- vreme i datum

// izabrani font sata; ako najširi zapis vremena ne staje na ekran (npr. sa sekundama na 4 modula) — uski
static uint8_t timeFontId() {
  uint8_t cols[64];
  const char* widest = cfg.showSeconds ? "00:00:00" : "00:00";
  if (renderColumns(cfg.timeFont, widest, cols, sizeof(cols)) <= displayCols) return cfg.timeFont;
  return FONT_ID_TIME_NARROW;
}

static void buildTime(char* out, size_t size) {
  if (!timeValid()) {
    strlcpySafe(out, cfg.showSeconds ? "--:--:--" : "--:--", size);
    return;
  }
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm t;
  localtime_r(&tv.tv_sec, &t);
  int h = cfg.h24 ? t.tm_hour : (t.tm_hour % 12 ? t.tm_hour % 12 : 12);
  char colon = (cfg.blinkColon && tv.tv_usec >= 500000) ? (char)glyphColonBlank() : ':';
  int n = snprintf_P(out, size, cfg.leadingZero ? PSTR("%02d%c%02d") : PSTR("%d%c%02d"), h, colon, t.tm_min);
  if (cfg.showSeconds && n > 0 && (size_t)n < size) snprintf_P(out + n, size - n, PSTR("%c%02d"), colon, t.tm_sec);
}

static void startTime() {
  item = IT_TIME;
  itemT0 = millis();
  buildTime(timeGlyphs, sizeof(timeGlyphs));
  P->setFont(fontData(timeFontId()));
  P->setCharSpacing(1);
  P->displayText(timeGlyphs, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
}

static void updateTime() {
  char buf[sizeof(timeGlyphs)];
  buildTime(buf, sizeof(buf));
  if (strcmp(buf, timeGlyphs) != 0) {
    strlcpySafe(timeGlyphs, buf, sizeof(timeGlyphs));
    P->displayReset();
  }
}

static bool buildDate(char* out, size_t size) {
  if (!timeValid()) return false;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char name[24];
  int y = t.tm_year + 1900, m = t.tm_mon + 1, d = t.tm_mday;
  switch (cfg.dateFormat) {
    default:
    case 0: snprintf_P(out, size, PSTR("%02d.%02d.%04d."), d, m, y); break;
    case 1: snprintf_P(out, size, PSTR("%02d.%02d."), d, m); break;
    case 2:
      monthName(name, sizeof(name), t.tm_mon, cfg.cyrillic, true);
      snprintf_P(out, size, PSTR("%d. %s %04d."), d, name, y);
      break;
    case 3:
      monthName(name, sizeof(name), t.tm_mon, cfg.cyrillic, false);
      snprintf_P(out, size, PSTR("%d. %s %04d."), d, name, y);
      break;
    case 4: snprintf_P(out, size, PSTR("%04d-%02d-%02d"), y, m, d); break;
    case 5:
      weekdayName(name, sizeof(name), t.tm_wday, cfg.cyrillic, true);
      snprintf_P(out, size, PSTR("%s %02d.%02d."), name, d, m);
      break;
  }
  return true;
}

static bool buildWeekday(char* out, size_t size) {
  if (!timeValid()) return false;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  weekdayName(out, size, t.tm_wday, cfg.cyrillic, false);
  return true;
}

// ---------------------------------------------------------------- rotacija

static void startPortalInfo() {
  utf8[0] = '\0';
  appendSys(utf8, sizeof(utf8), "Podešavanje: Wi-Fi ");
  appendRaw(utf8, sizeof(utf8), portalSsid());
  appendSys(utf8, sizeof(utf8), ", lozinka ");
  appendRaw(utf8, sizeof(utf8), cfg.apPass);
  appendRaw(utf8, sizeof(utf8), ", http://");
  appendRaw(utf8, sizeof(utf8), portalIp());
  lastPortalInfo = millis();
  startUtf8(IT_PORTAL, utf8, FONT_ID_TEXT, 1000);
}

static bool startNextMessage() {
  while (msgCursor < MSG_COUNT) {
    const Message& m = cfg.msgs[msgCursor++];
    if (m.enabled && m.text[0]) {
      startUtf8(IT_MESSAGE, m.text, FONT_ID_TEXT, cfg.infoSeconds * 1000U);
      return true;
    }
  }
  return false;
}

// vrednosti senzora jedna za drugom; nevalidne ili zastarele se preskaču (nikad NaN na matrici)
static bool startNextSensorValue() {
  while (sensorCursor < 3) {
    uint8_t i = sensorCursor++;
    float v;
    char b[24];
    if (i == 0 && cfg.sens.showTemp && sensorTemperature(v)) snprintf_P(b, sizeof(b), PSTR("%.1f°C"), v);
    else if (i == 1 && cfg.sens.showHum && sensorHumidity(v)) snprintf_P(b, sizeof(b), PSTR("%.0f%%"), v);
    else if (i == 2 && cfg.sens.showPress && sensorPressure(v)) snprintf_P(b, sizeof(b), PSTR("%.0f hPa"), v);
    else continue;
    startUtf8(IT_SENSOR, b, FONT_ID_TEXT, cfg.infoSeconds * 1000U);
    return true;
  }
  return false;
}

static bool startPage(uint8_t page) {
  switch (page) {
    case PAGE_TIME:
      startTime();
      return true;
    case PAGE_DATE:
      if (!buildDate(utf8, sizeof(utf8))) return false;
      startUtf8(IT_DATE, utf8, FONT_ID_TEXT, cfg.infoSeconds * 1000U);
      return true;
    case PAGE_WEEKDAY:
      if (!buildWeekday(utf8, sizeof(utf8))) return false;
      startUtf8(IT_WEEKDAY, utf8, FONT_ID_TEXT, cfg.infoSeconds * 1000U);
      return true;
    case PAGE_MESSAGES:
      if (mode == DM_DND) return false;
      inMessages = true;
      msgCursor = 0;
      return startNextMessage();
    case PAGE_SENSOR:
      inSensor = true;
      sensorCursor = 0;
      return startNextSensorValue();
  }
  return false;
}

static void nextItem() {
  uint32_t now = millis();
  bool dnd = mode == DM_DND;
  if (forcedPage >= 0) {
    uint8_t p = (uint8_t)forcedPage;
    forcedPage = -1;
    if (startPage(p)) return;
  }
  if (prioActive && dnd) {
    if ((uint32_t)(now - prioT0) >= prioMs) prioActive = false;
  } else if (prioActive) {
    if ((uint32_t)(now - prioT0) < prioMs) {
      startUtf8(IT_PRIORITY, prio, FONT_ID_TEXT, cfg.infoSeconds * 1000U);
      return;
    }
    prioActive = false;
    LOG("Prikaz: prioritetna poruka istekla");
  }
  if (noticePending && !dnd) {
    noticePending = false;
    startUtf8(IT_NOTICE, notice, FONT_ID_TEXT, 2500);
    return;
  }
  if (portalActive() && (lastPortalInfo == 0 || elapsed(lastPortalInfo, PORTAL_INFO_EVERY_MS))) {
    startPortalInfo();
    return;
  }

  // najviše dva kruga kroz stranice; ako ništa nije uključeno — vreme
  for (uint8_t guard = 0; guard < PAGE_COUNT * 2; guard++) {
    if (inMessages) {
      if (!dnd && startNextMessage()) return;
      inMessages = false;
    }
    if (inSensor) {
      if (startNextSensorValue()) return;
      inSensor = false;
    }
    uint8_t page = cfg.pageOrder[pageCursor];
    pageCursor = (pageCursor + 1) % PAGE_COUNT;
    if (cfg.pageEnabled[page] && startPage(page)) return;
  }
  startTime();
}

// ---------------------------------------------------------------- osvetljaj

static void updateBrightness(bool force) {
  uint8_t target = cfg.brightDay;
  bool off = false;
  bool night = false;
  if (cfg.nightEnabled && timeValid() && cfg.nightStart != cfg.nightEnd) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    uint16_t m = t.tm_hour * 60 + t.tm_min;
    night = cfg.nightStart < cfg.nightEnd ? (m >= cfg.nightStart && m < cfg.nightEnd)
                                          : (m >= cfg.nightStart || m < cfg.nightEnd);
    if (night) {
      if (cfg.nightMode == NIGHT_OFF) off = true;
      else target = cfg.brightNight;
    }
  }
  // ručni nivo važi do sledeće smene dan/noć, zatim raspored ponovo preuzima
  if (lastNight >= 0 && (int8_t)night != lastNight) brightOverride = -1;
  lastNight = night;
  if (brightOverride >= 0) target = (uint8_t)brightOverride;
  if (target > cfg.brightMax) target = cfg.brightMax;
  if (mode == DM_OFF) off = true;
  if (item == IT_TEST) off = false;
  if (force || target != brightness) {
    brightness = target;
    P->setIntensity(brightness);
  }
  if (force || off == isOn) {
    isOn = !off;
    P->displayShutdown(off);
    LOG("Prikaz: %s, osvetljaj %u", isOn ? "uključen" : "isključen", brightness);
  }
}

// ---------------------------------------------------------------- test ekran

static void drawTest() {
  MD_MAX72XX* mx = P->getGraphicObject();
  mx->control(MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);
  mx->clear();
  if (testPhase == 0) {
    for (uint16_t c = 0; c < displayCols; c++) mx->setColumn(c, 0xFF);
  } else {
    // broj svakog modula, s leva na desno; kolona 0 je krajnja desna.
    // dvocifreni brojevi uskim fontom da stanu u 8 kolona
    for (uint8_t m = 0; m < modulesActive; m++) {
      char g[3];
      snprintf_P(g, sizeof(g), PSTR("%u"), m + 1);
      uint8_t cols[8];
      uint16_t w = renderColumns(m < 9 ? FONT_ID_TIME_STD : FONT_ID_TIME_NARROW, g, cols, sizeof(cols));
      uint16_t x0 = m * 8 + (8 - w) / 2;
      for (uint16_t i = 0; i < w; i++) mx->setColumn(displayCols - 1 - (x0 + i), cols[i]);
    }
  }
  mx->control(MD_MAX72XX::UPDATE, MD_MAX72XX::ON);
}

void displayTest() {
  LOG("Prikaz: test ekran");
  item = IT_TEST;
  testPhase = 0;
  testT0 = millis();
  updateBrightness(true);
  drawTest();
}

static void loopTest() {
  if (testPhase == 0 && elapsed(testT0, 2500)) {
    testPhase = 1;
    testT0 = millis();
    drawTest();
  } else if (testPhase == 1 && elapsed(testT0, 5000)) {
    P->displayClear();
    item = IT_NONE;
    updateBrightness(true);
    nextItem();
  }
}

// ---------------------------------------------------------------- javni API

void displayBegin() {
  hwActive = cfg.hwType;
  modulesActive = cfg.modules;
  displayCols = modulesActive * 8;
  P = new MD_Parola((MD_MAX72XX::moduleType_t)hwActive, PIN_MAX_DIN, PIN_MAX_CLK, PIN_MAX_CS, modulesActive);
  P->begin();
  P->setIntensity(0);  // bezbedan početak
  P->displayClear();
  brightness = 0;
  bootMs = millis();
  LOG("Prikaz: MAX7219 x%u (%u kolona), tip modula %u", modulesActive, displayCols, hwActive);
  nextItem();
}

void displayApplyConfig() {
  if (startupDone) updateBrightness(true);
  inMessages = inSensor = false;
  pageCursor = 0;
  if (item != IT_TEST) nextItem();
}

void displayLoop() {
  if (!P) return;
  static uint32_t lastSlow = 0;

  if (!startupDone && elapsed(bootMs, STARTUP_DARK_MS)) {
    startupDone = true;
    updateBrightness(true);
  }
  if (elapsed(lastSlow, 1000)) {
    lastSlow = millis();
    if (startupDone) updateBrightness(false);
    TimeStatus ts = timeStatus();
    if (ts == TIME_UNAVAILABLE && prevTimeStatus != TIME_UNAVAILABLE &&
        (lastNtpWarn == 0 || elapsed(lastNtpWarn, NTP_WARN_EVERY_MS))) {
      lastNtpWarn = millis();
      displayNotice("NTP nije dostupan", true);
    }
    prevTimeStatus = ts;
  }

  if (item == IT_TEST) {
    loopTest();
    return;
  }

  bool done = P->displayAnimate();
  if (item == IT_TIME) {
    if (done) updateTime();
    // vreme traje cfg.timeSeconds; prioritetna poruka ga prekida odmah
    if ((prioActive && mode != DM_DND) || elapsed(itemT0, cfg.timeSeconds * 1000UL)) nextItem();
  } else if (done) {
    nextItem();
  }
}

void displayNotice(const char* text, bool translit) {
  notice[0] = '\0';
  if (translit) appendSys(notice, sizeof(notice), text);
  else strlcpySafe(notice, text, sizeof(notice));
  noticePending = true;
  LOG("Prikaz: obaveštenje '%s'", notice);
}

void displayPriority(const char* text, uint16_t seconds) {
  strlcpySafe(prio, text, sizeof(prio));
  prioActive = prio[0] != '\0';
  prioT0 = millis();
  prioMs = seconds * 1000UL;
  LOG("Prikaz: prioritetna poruka %us", seconds);
  if (prioActive && item != IT_TEST) nextItem();
}

void displayPriorityClear() {
  prioActive = false;
}

void displaySetMode(DisplayMode m) {
  if (m >= DM_COUNT || m == mode) return;
  mode = m;
  LOG("Prikaz: režim '%s'", displayModeKey(m));
  updateBrightness(true);
  if (m == DM_DND && (item == IT_MESSAGE || item == IT_PRIORITY || item == IT_NOTICE)) nextItem();
}

DisplayMode displayMode() { return mode; }

const char* displayModeKey(DisplayMode m) {
  switch (m) {
    case DM_OFF: return "off";
    case DM_DND: return "dnd";
    default: return "normal";
  }
}

bool displayModeFromKey(const char* key, DisplayMode& out) {
  for (uint8_t i = 0; i < DM_COUNT; i++) {
    if (!strcmp(key, displayModeKey((DisplayMode)i))) {
      out = (DisplayMode)i;
      return true;
    }
  }
  return false;
}

void displayShowPage(uint8_t page) {
  if (page >= PAGE_COUNT) return;
  forcedPage = page;
  inMessages = inSensor = false;
  if (item != IT_TEST) nextItem();
}

void displaySetBrightness(uint8_t level) {
  brightOverride = level > 15 ? 15 : level;
  updateBrightness(false);
}

uint8_t displayBrightness() { return brightness; }
bool displayIsOn() { return isOn; }
uint8_t displayHwTypeActive() { return hwActive; }
uint8_t displayModulesActive() { return modulesActive; }

const char* displayItemName() {
  switch (item) {
    case IT_TIME: return "vreme";
    case IT_DATE: return "datum";
    case IT_WEEKDAY: return "dan u nedelji";
    case IT_MESSAGE: return "poruka";
    case IT_SENSOR: return "senzor";
    case IT_NOTICE: return "obaveštenje";
    case IT_PRIORITY: return "prioritetna poruka";
    case IT_PORTAL: return "uputstvo za Wi-Fi";
    case IT_TEST: return "test ekran";
    default: return "-";
  }
}
