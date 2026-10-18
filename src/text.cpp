#include "text.h"

#include "app.h"
#include "generated/fonts.h"

static_assert(FONT_COUNT == 4, "FontId enum ne odgovara tools/gen_fonts.py");

const uint8_t* fontData(uint8_t id) { return FONT_TABLE[id < FONT_COUNT ? id : 0]; }
const char* fontName(uint8_t id) { return FONT_NAMES[id < FONT_COUNT ? id : 0]; }
uint8_t fontCount() { return FONT_COUNT; }
char glyphDateDot(uint8_t style) {
  switch (style) {
    case 0: return (char)GLYPH_DOT1;
    case 2: return (char)GLYPH_DOT4;
    default: return (char)GLYPH_DOT2;
  }
}

char glyphColon(uint8_t style, bool blank) {
  switch (style) {
    case 0: return (char)(blank ? GLYPH_COLON1_BLANK : GLYPH_COLON1);
    case 2: return (char)(blank ? GLYPH_COLON4_BLANK : GLYPH_COLON4);
    default: return (char)(blank ? GLYPH_COLON2_BLANK : GLYPH_COLON2);
  }
}
uint8_t glyphDegree() { return GLYPH_DEGREE; }

// ---------------------------------------------------------------- UTF-8

static uint8_t lookupCodepoint(uint32_t cp) {
  if (cp >= 0x20 && cp <= 0x7E) return (uint8_t)cp;
  if (cp > 0xFFFF) return GLYPH_REPLACEMENT;
  int lo = 0, hi = UNI_MAP_LEN - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    uint16_t mcp = pgm_read_word(&UNI_MAP[mid].cp);
    if (mcp == cp) return pgm_read_byte(&UNI_MAP[mid].code);
    if (mcp < cp) lo = mid + 1;
    else hi = mid - 1;
  }
  return GLYPH_REPLACEMENT;
}

size_t utf8ToGlyphs(const char* in, char* out, size_t outSize) {
  if (!outSize) return 0;
  size_t n = 0;
  const uint8_t* p = (const uint8_t*)(in ? in : "");
  while (*p && n + 1 < outSize) {
    uint32_t cp;
    uint8_t b = *p;
    uint8_t extra;
    if (b < 0x80) { cp = b; extra = 0; }
    else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; extra = 1; }
    else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; extra = 2; }
    else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; extra = 3; }
    else { p++; out[n++] = (char)GLYPH_REPLACEMENT; continue; }  // nastavni bajt bez početka
    p++;
    bool ok = true;
    for (uint8_t i = 0; i < extra; i++) {
      if ((*p & 0xC0) != 0x80) { ok = false; break; }  // prekinuta sekvenca
      cp = (cp << 6) | (*p++ & 0x3F);
    }
    if (!ok) { out[n++] = (char)GLYPH_REPLACEMENT; continue; }
    if (cp < 0x20 || cp == 0x7F) { out[n++] = ' '; continue; }  // kontrolni znakovi
    if (cp == 0xA0) cp = ' ';                                     // tvrdi razmak
    out[n++] = (char)lookupCodepoint(cp);
  }
  out[n] = '\0';
  return n;
}

// ---------------------------------------------------------------- iscrtavanje

static int32_t glyphOffset(const uint8_t* font, uint8_t code) {
  uint8_t first = pgm_read_byte(font + 2), last = pgm_read_byte(font + 3);
  if (code < first || code > last) return -1;
  int32_t off = 5;
  for (uint16_t c = first; c < code; c++) off += pgm_read_byte(font + off) + 1;
  return off;
}

uint16_t renderColumns(uint8_t fontId, const char* glyphs, uint8_t* cols, uint16_t maxCols, uint8_t spacing) {
  const uint8_t* font = fontData(fontId);
  uint16_t n = 0;
  for (const uint8_t* p = (const uint8_t*)glyphs; *p; p++) {
    int32_t off = glyphOffset(font, *p);
    if (off < 0) off = glyphOffset(font, GLYPH_REPLACEMENT);
    uint8_t w = pgm_read_byte(font + off);
    if (w == 0) continue;
    if (n && spacing) {
      for (uint8_t s = 0; s < spacing && n < maxCols; s++) cols[n++] = 0;
    }
    for (uint8_t i = 0; i < w && n < maxCols; i++) cols[n++] = pgm_read_byte(font + off + 1 + i);
    if (n >= maxCols) break;
  }
  return n;
}

// ---------------------------------------------------------------- nazivi

static const char WD_LAT[] PROGMEM = "Nedelja|Ponedeljak|Utorak|Sreda|Četvrtak|Petak|Subota";
static const char WD_CYR[] PROGMEM = "Недеља|Понедељак|Уторак|Среда|Четвртак|Петак|Субота";
static const char WDA_LAT[] PROGMEM = "ned|pon|uto|sre|čet|pet|sub";
static const char WDA_CYR[] PROGMEM = "нед|пон|уто|сре|чет|пет|суб";
static const char MON_LAT[] PROGMEM =
    "januar|februar|mart|april|maj|jun|jul|avgust|septembar|oktobar|novembar|decembar";
static const char MON_CYR[] PROGMEM =
    "јануар|фебруар|март|април|мај|јун|јул|август|септембар|октобар|новембар|децембар";
static const char MONA_LAT[] PROGMEM = "jan|feb|mar|apr|maj|jun|jul|avg|sep|okt|nov|dec";
static const char MONA_CYR[] PROGMEM = "јан|феб|мар|апр|мај|јун|јул|авг|сеп|окт|нов|дец";

// izdvaja idx-ti element liste razdvojene sa '|' iz PROGMEM stringa
static void pickItem(char* dst, size_t size, PGM_P list, int idx) {
  size_t n = 0;
  int cur = 0;
  for (PGM_P p = list;; p++) {
    char ch = pgm_read_byte(p);
    if (ch == '\0' || ch == '|') {
      if (cur == idx || ch == '\0') break;
      cur++;
      continue;
    }
    if (cur == idx && n + 1 < size) dst[n++] = ch;
  }
  if (size) dst[n] = '\0';
}

void weekdayName(char* dst, size_t size, int wday, bool cyr, bool abbr) {
  if (wday < 0 || wday > 6) wday = 0;
  pickItem(dst, size, abbr ? (cyr ? WDA_CYR : WDA_LAT) : (cyr ? WD_CYR : WD_LAT), wday);
}

void monthName(char* dst, size_t size, int mon, bool cyr, bool abbr) {
  if (mon < 0 || mon > 11) mon = 0;
  pickItem(dst, size, abbr ? (cyr ? MONA_CYR : MONA_LAT) : (cyr ? MON_CYR : MON_LAT), mon);
}

// ---------------------------------------------------------------- transliteracija

struct Translit {
  const char* lat;
  const char* cyr;
};
// dvoznaci moraju biti pre jednoznaka
static const Translit TRANSLIT[] PROGMEM = {
    {"Lj", "Љ"}, {"LJ", "Љ"}, {"lj", "љ"}, {"Nj", "Њ"}, {"NJ", "Њ"}, {"nj", "њ"},
    {"Dž", "Џ"}, {"DŽ", "Џ"}, {"dž", "џ"},
    {"A", "А"}, {"B", "Б"}, {"V", "В"}, {"G", "Г"}, {"D", "Д"}, {"Đ", "Ђ"}, {"E", "Е"},
    {"Ž", "Ж"}, {"Z", "З"}, {"I", "И"}, {"J", "Ј"}, {"K", "К"}, {"L", "Л"}, {"M", "М"},
    {"N", "Н"}, {"O", "О"}, {"P", "П"}, {"R", "Р"}, {"S", "С"}, {"T", "Т"}, {"Ć", "Ћ"},
    {"U", "У"}, {"F", "Ф"}, {"H", "Х"}, {"C", "Ц"}, {"Č", "Ч"}, {"Š", "Ш"},
    {"a", "а"}, {"b", "б"}, {"v", "в"}, {"g", "г"}, {"d", "д"}, {"đ", "ђ"}, {"e", "е"},
    {"ž", "ж"}, {"z", "з"}, {"i", "и"}, {"j", "ј"}, {"k", "к"}, {"l", "л"}, {"m", "м"},
    {"n", "н"}, {"o", "о"}, {"p", "п"}, {"r", "р"}, {"s", "с"}, {"t", "т"}, {"ć", "ћ"},
    {"u", "у"}, {"f", "ф"}, {"h", "х"}, {"c", "ц"}, {"č", "ч"}, {"š", "ш"},
};

void latinToCyrillic(const char* in, char* out, size_t outSize) {
  size_t n = 0;
  while (*in && n + 1 < outSize) {
    bool done = false;
    for (const Translit& t : TRANSLIT) {
      size_t l = strlen(t.lat);
      if (strncmp(in, t.lat, l) == 0) {
        size_t cl = strlen(t.cyr);
        if (n + cl >= outSize) { out[n] = '\0'; return; }
        memcpy(out + n, t.cyr, cl);
        n += cl;
        in += l;
        done = true;
        break;
      }
    }
    if (!done) out[n++] = *in++;
  }
  out[n] = '\0';
}
