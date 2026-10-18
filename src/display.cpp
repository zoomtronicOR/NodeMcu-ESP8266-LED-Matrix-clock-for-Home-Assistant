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
  IT_NONE, IT_BOOT, IT_TIME, IT_DATE, IT_WEEKDAY, IT_MESSAGE, IT_SENSOR, IT_NOTICE, IT_PRIORITY, IT_PORTAL, IT_TEST,
  IT_HOLD
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
  if (it == IT_DATE) {
    // tačke u datumu u izabranoj veličini (obična '.' u porukama ostaje)
    char dot = glyphDateDot(cfg.dateDot);
    for (char* p = glyphs; *p; p++)
      if (*p == '.') *p = dot;
  }
  showGlyphs(font, holdMs);
}

// ---------------------------------------------------------------- vreme i datum

// Raspored vremena prema širini ekrana (računa se na početku stranice vremena):
// sekunde samo ako staju makar uskim fontom (na 3 modula ne staju); izabrani font sata ako
// staje, inače uski.
static uint8_t tlFont = FONT_ID_TIME_NARROW;
static bool tlSec = false;

static uint16_t textWidth(uint8_t font, const char* glyphStr) {
  uint8_t cols[MODULES_MAX * 8];
  return renderColumns(font, glyphStr, cols, sizeof(cols));
}

static void computeTimeLayout() {
  char c = glyphColon(cfg.colonStyle, false);
  char hm[6] = {'0', '0', c, '0', '0', 0};
  char hms[9] = {'0', '0', c, '0', '0', c, '0', '0', 0};
  tlSec = cfg.showSeconds && textWidth(FONT_ID_TIME_NARROW, hms) <= displayCols;
  tlFont = textWidth(cfg.timeFont, tlSec ? hms : hm) <= displayCols ? cfg.timeFont : (uint8_t)FONT_ID_TIME_NARROW;
}

static void buildTime(char* out, size_t size) {
  if (!timeValid()) {
    char c = glyphColon(cfg.colonStyle, false);
    snprintf_P(out, size, tlSec ? PSTR("--%c--%c--") : PSTR("--%c--"), c, c);
    return;
  }
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm t;
  localtime_r(&tv.tv_sec, &t);
  int h = cfg.h24 ? t.tm_hour : (t.tm_hour % 12 ? t.tm_hour % 12 : 12);
  char colon = glyphColon(cfg.colonStyle, cfg.blinkColon && tv.tv_usec >= 500000);
  int n = snprintf_P(out, size, cfg.leadingZero ? PSTR("%02d%c%02d") : PSTR("%d%c%02d"), h, colon, t.tm_min);
  if (tlSec && n > 0 && (size_t)n < size) snprintf_P(out + n, size - n, PSTR("%c%02d"), colon, t.tm_sec);
}

// ---------------------------------------------------------------- vreme sa animacijom cifara
// Kad je uključena animacija cifara, vreme crta ovaj modul direktno (Parola ne animira deo
// teksta): cifra koja se promenila "otkotrlja" se nagore ili nadole u TIME_ANIM_STEPS koraka.
// Sati, minuti i sekunde se biraju posebno (cfg.animHour/animMin/animSec).

constexpr uint8_t TIME_ANIM_STEPS = 8;      // visina matrice
constexpr uint32_t TIME_ANIM_STEP_MS = 30;  // cela animacija ≈ 0,25 s

static bool timeCustom = false;
static char tcFrom[12];                     // vreme pre promene (polazne cifre animacije)
static uint8_t tcStep = TIME_ANIM_STEPS;    // >= TIME_ANIM_STEPS: nema animacije u toku
static uint32_t tcStepT = 0;

static bool timeCustomWanted() {
  return cfg.animDir != ANIM_NONE && (cfg.animHour || cfg.animMin || (cfg.animSec && tlSec));
}

static bool isDigitGlyph(char c) { return c >= '0' && c <= '9'; }

// da li se cifra na poziciji i animira: grupa po dvotačkama (0 = sati, 1 = minuti, 2 = sekunde)
static bool rollAllowed(const char* s, uint8_t i) {
  uint8_t group = 0;
  for (uint8_t j = 0; j < i; j++)
    if (!isDigitGlyph(s[j]) && s[j] != '-') group++;
  return group == 0 ? cfg.animHour : group == 1 ? cfg.animMin : cfg.animSec;
}

static void drawTimeFrame() {
  uint8_t font = tlFont;
  uint8_t frame[MODULES_MAX * 8];
  memset(frame, 0, sizeof(frame));
  bool anim = tcStep < TIME_ANIM_STEPS && strlen(tcFrom) == strlen(timeGlyphs);
  uint8_t k = anim ? tcStep : TIME_ANIM_STEPS;

  // širina radi centriranja (razmak 1 kolona između znakova, kao Parola)
  uint8_t tmp[MODULES_MAX * 8];
  uint16_t total = renderColumns(font, timeGlyphs, tmp, sizeof(tmp));
  int16_t x = total < displayCols ? (displayCols - total) / 2 : 0;

  for (uint8_t i = 0; timeGlyphs[i]; i++) {
    char cur[2] = {timeGlyphs[i], 0};
    uint8_t now[8], old[8];
    uint16_t w = renderColumns(font, cur, now, sizeof(now), 0);
    bool roll = anim && tcFrom[i] != timeGlyphs[i] && isDigitGlyph(tcFrom[i]) && isDigitGlyph(timeGlyphs[i]) &&
                rollAllowed(timeGlyphs, i);
    if (roll) {
      char prev[2] = {tcFrom[i], 0};
      renderColumns(font, prev, old, sizeof(old), 0);  // cifre su iste širine
    }
    for (uint16_t c = 0; c < w && x + (int16_t)c < (int16_t)displayCols; c++) {
      uint16_t v = now[c];
      if (roll) {
        // bit0 = gornji red: nagore = stara ide gore (>>), nova dolazi odozdo (<<)
        v = cfg.animDir == ANIM_UP ? (old[c] >> k) | (now[c] << (TIME_ANIM_STEPS - k))
                                   : (old[c] << k) | (now[c] >> (TIME_ANIM_STEPS - k));
      }
      if (x + (int16_t)c >= 0) frame[x + c] = (uint8_t)v;
    }
    x += w + 1;
  }

  MD_MAX72XX* mx = P->getGraphicObject();
  mx->control(MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);
  for (uint16_t c = 0; c < displayCols; c++) mx->setColumn(displayCols - 1 - c, frame[c]);  // kolona 0 = desno
  mx->control(MD_MAX72XX::UPDATE, MD_MAX72XX::ON);
}

static void timeCustomLoop() {
  char now[sizeof(timeGlyphs)];
  buildTime(now, sizeof(now));
  if (strcmp(now, timeGlyphs) != 0) {
    bool digitChanged = false;
    if (strlen(now) == strlen(timeGlyphs))
      for (uint8_t i = 0; now[i]; i++)
        if (now[i] != timeGlyphs[i] && isDigitGlyph(now[i]) && isDigitGlyph(timeGlyphs[i]) && rollAllowed(now, i))
          digitChanged = true;
    if (digitChanged) {  // animacija od prethodnog vremena (trepćuća dvotačka se ne animira)
      strlcpySafe(tcFrom, timeGlyphs, sizeof(tcFrom));
      tcStep = 0;
      tcStepT = millis();
    }
    strlcpySafe(timeGlyphs, now, sizeof(timeGlyphs));
    drawTimeFrame();
  }
  if (tcStep < TIME_ANIM_STEPS && elapsed(tcStepT, TIME_ANIM_STEP_MS)) {
    tcStepT = millis();
    tcStep++;
    drawTimeFrame();
  }
}

static void startTime() {
  item = IT_TIME;
  itemT0 = millis();
  computeTimeLayout();
  buildTime(timeGlyphs, sizeof(timeGlyphs));
  timeCustom = timeCustomWanted();
  if (timeCustom) {
    tcStep = TIME_ANIM_STEPS;
    drawTimeFrame();
    return;
  }
  P->setFont(fontData(tlFont));
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
    case PAGE_DATE: {
      if (!buildDate(utf8, sizeof(utf8))) return false;
      // uski font ima samo cifre, '.', '-' i dvotačke: samo za brojčane formate
      bool numeric = cfg.dateFormat == 0 || cfg.dateFormat == 1 || cfg.dateFormat == 4;
      startUtf8(IT_DATE, utf8, cfg.dateNarrow && numeric ? FONT_ID_TIME_NARROW : FONT_ID_TEXT,
                cfg.infoSeconds * 1000U);
      return true;
    }
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

// ---------------------------------------------------------------- animacija pri pokretanju

// Jedan piksel trči u krug po obodu matrice dok sistem ne bude spreman: vreme sinhronizovano,
// potrebno uputstvo za Wi-Fi podešavanje, ili najduže BOOT_ANIM_MAX_MS (sat ne zavisi od mreže).
constexpr uint32_t BOOT_ANIM_MAX_MS = 30000;
constexpr uint32_t BOOT_STEP_MS = 13;  // krug na 4 modula (76 piksela) ≈ 1 s

static uint16_t bootPos = 0;
static uint32_t bootStepT = 0;
static int16_t bootPrevX = -1;
static uint8_t bootPrevY = 0;

static uint16_t perimeterLen() { return 2 * displayCols + 12; }  // 2·širina + 2·8 − 4 ugla

// i-ta tačka oboda, u smeru kazaljke na satu od gornjeg levog ugla (x = 0 levo, y = 0 gore)
static void perimeterPoint(uint16_t i, uint16_t& x, uint8_t& y) {
  uint16_t w = displayCols;
  if (i < w) { x = i; y = 0; return; }
  i -= w;
  if (i < 7) { x = w - 1; y = i + 1; return; }
  i -= 7;
  if (i < w - 1) { x = w - 2 - i; y = 7; return; }
  i -= w - 1;
  x = 0;
  y = 6 - i;
}

static void drawBootPixel() {
  MD_MAX72XX* mx = P->getGraphicObject();
  uint16_t x;
  uint8_t y;
  perimeterPoint(bootPos, x, y);
  // kolona 0 je krajnja desna
  if (bootPrevX >= 0) mx->setPoint(bootPrevY, displayCols - 1 - bootPrevX, false);
  mx->setPoint(y, displayCols - 1 - x, true);
  bootPrevX = x;
  bootPrevY = y;
}

static void loopBoot() {
  if (timeValid() || portalActive() || elapsed(bootMs, BOOT_ANIM_MAX_MS)) {
    P->displayClear();
    item = IT_NONE;
    nextItem();
    return;
  }
  if (elapsed(bootStepT, BOOT_STEP_MS)) {
    bootStepT = millis();
    bootPos = (bootPos + 1) % perimeterLen();
    drawBootPixel();
  }
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
  item = IT_BOOT;  // animacija dok sistem ne bude spreman (loopBoot)
  bootStepT = millis();
  drawBootPixel();
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
  if (item == IT_HOLD) return;
  if (item == IT_BOOT) {
    loopBoot();
    return;
  }

  if (item == IT_TIME && timeCustom) {
    timeCustomLoop();
    if ((prioActive && mode != DM_DND) || elapsed(itemT0, cfg.timeSeconds * 1000UL)) nextItem();
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

void displayHold(const char* text) {
  if (!P) return;
  item = IT_HOLD;
  utf8ToGlyphs(text, glyphs, sizeof(glyphs));
  P->setFont(fontData(FONT_ID_TEXT));
  P->setCharSpacing(1);
  P->displayText(glyphs, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  if (!isOn) P->displayShutdown(false);  // i noću: korisnik treba da vidi da traje ažuriranje
  // PA_PRINT se iscrta u prvom prolazu; drugi poziv za svaki slučaj
  P->displayAnimate();
  P->displayAnimate();
}

void displayResume() {
  if (item != IT_HOLD) return;
  item = IT_NONE;
  updateBrightness(true);
  nextItem();
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
    case IT_HOLD: return "ažuriranje";
    case IT_BOOT: return "pokretanje";
    default: return "-";
  }
}
