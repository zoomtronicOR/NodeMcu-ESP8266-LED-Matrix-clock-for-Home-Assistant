// Tekst za matricu: UTF-8 -> kodovi glifova, srpski nazivi, pristup fontovima.
#pragma once
#include <Arduino.h>

enum FontId : uint8_t { FONT_ID_TEXT = 0, FONT_ID_TIME_STD, FONT_ID_TIME_BOLD, FONT_ID_TIME_NARROW };

const uint8_t* fontData(uint8_t id);  // PROGMEM, format MD_MAX72XX 'F' v1
const char* fontName(uint8_t id);
uint8_t fontCount();
// dvotačka sata: style 0 = 1+1, 1 = 2+2, 2 = 4+4 piksela; blank = prazna iste širine (treptanje)
char glyphColon(uint8_t style, bool blank);
// tačka u datumu: style 0 = 1, 1 = 2, 2 = 4 piksela
char glyphDateDot(uint8_t style);
uint8_t glyphDegree();

// UTF-8 -> jednobajtni kodovi fonta. Nepoznati znakovi -> zamenski glif, neispravan
// UTF-8 se preskače kontrolisano. Uvek terminira izlaz. Vraća broj znakova.
size_t utf8ToGlyphs(const char* in, char* out, size_t outSize);

// Iscrtava tekst (kodovi glifova) u kolone; bit0 = gornji red. Vraća broj kolona.
uint16_t renderColumns(uint8_t font, const char* glyphs, uint8_t* cols, uint16_t maxCols, uint8_t spacing = 1);

// Srpski nazivi (UTF-8) u dst. wday 0 = nedelja; mon 0 = januar.
void weekdayName(char* dst, size_t size, int wday, bool cyr, bool abbr);
void monthName(char* dst, size_t size, int mon, bool cyr, bool abbr);

// Latinični srpski tekst -> ćirilica (lj, nj, dž kao jedno slovo). Za sistemske poruke.
void latinToCyrillic(const char* in, char* out, size_t outSize);
// Ćirilica -> latinica (npr. opis vremena sa servera kad je izabrana latinica)
void cyrillicToLatin(const char* in, char* out, size_t outSize);
