#include "config.h"

#include <LittleFS.h>

#include <memory>

#include "app.h"

Config cfg;
char configError[96] = "";

static const char CONFIG_FILE[] = "/config.json";
static const char CONFIG_TMP[] = "/config.tmp";

// fontovi sata u FONT_TABLE (vidi tools/gen_fonts.py)
constexpr uint8_t TIME_FONT_FIRST = 1;
constexpr uint8_t TIME_FONT_LAST = 3;

void configDefaults(Config& c) {
  memset(&c, 0, sizeof(c));
  snprintf_P(c.hostname, sizeof(c.hostname), PSTR("matrixclock-%04x"), (unsigned)(ESP.getChipId() & 0xFFFF));
  strlcpySafe(c.apPass, "matrica123", sizeof(c.apPass));
  c.portalAfterFails = 5;

  strlcpySafe(c.tzName, "Europe/Belgrade", sizeof(c.tzName));
  strlcpySafe(c.tz, "CET-1CEST,M3.5.0,M10.5.0/3", sizeof(c.tz));
  strlcpySafe(c.ntp[0], "pool.ntp.org", sizeof(c.ntp[0]));
  strlcpySafe(c.ntp[1], "time.google.com", sizeof(c.ntp[1]));
  strlcpySafe(c.ntp[2], "time.cloudflare.com", sizeof(c.ntp[2]));
  c.h24 = true;
  c.showSeconds = false;
  c.animDir = ANIM_UP;
  c.animHour = c.animMin = c.animSec = true;
  c.blinkColon = true;
  c.leadingZero = true;
  c.dateFormat = 0;
  c.dateDot = 0;  // 1 piksel
  c.dateNarrow = false;
  c.cyrillic = false;

  c.hwType = 1;  // FC16_HW — najčešći 4-u-1 moduli
  c.modules = 4;
  c.timeFont = 1;
  c.colonStyle = 1;  // 2+2
  c.brightDay = 4;
  c.brightNight = 1;
  c.brightMax = 10;
  c.nightEnabled = true;
  c.nightStart = 22 * 60;
  c.nightEnd = 7 * 60;
  c.nightMode = NIGHT_DIM;
  c.scrollSpeed = 40;
  c.scrollPause = 1000;
  c.scrollDir = SCROLL_LEFT;
  c.timeSeconds = 15;
  c.infoSeconds = 4;
  for (uint8_t i = 0; i < PAGE_COUNT; i++) {
    c.pageOrder[i] = i;
    c.pageEnabled[i] = true;
  }
  c.msgs[0].enabled = true;
  strlcpySafe(c.msgs[0].text, "Matrični sat V3 — čćžšđ ČĆŽŠĐ", sizeof(c.msgs[0].text));

  c.mqtt.enabled = false;  // isključen dok korisnik ne unese broker
  c.mqtt.port = 1883;
  snprintf_P(c.mqtt.base, sizeof(c.mqtt.base), PSTR("matrix_clock_%04x"), (unsigned)(ESP.getChipId() & 0xFFFF));
  c.mqtt.discovery = true;
  strlcpySafe(c.mqtt.discPrefix, "homeassistant", sizeof(c.mqtt.discPrefix));
  c.mqtt.teleSeconds = 120;

  c.sens.type = 0;  // nijedan
  c.sens.dhtPin = 16;
  c.sens.interval = 30;
  c.sens.showTemp = c.sens.showHum = c.sens.showPress = true;

  c.owm.enabled = false;
  strlcpySafe(c.owm.city, "Beograd,RS", sizeof(c.owm.city));
  c.owm.intervalMin = 15;
  strlcpySafe(c.owm.label, "Napolju", sizeof(c.owm.label));
  c.owm.showTemp = true;
}

// ---------------------------------------------------------------- validacija

namespace {

struct Ctx {
  char* err;
  size_t len;
  bool ok = true;
  void fail(const char* field, const __FlashStringHelper* msg) {
    if (!ok) return;  // zadrži prvu grešku
    ok = false;
    snprintf_P(err, len, PSTR("%s: %s"), field, (PGM_P)msg);
  }
};

bool isHostChar(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-'; }
bool isServerChar(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.';
}
// MQTT tema: slova, cifre, '_', '-', '/'; bez '/' na početku/kraju, bez praznih nivoa i džokera
bool validTopic(const char* t) {
  size_t l = strlen(t);
  if (l == 0 || t[0] == '/' || t[l - 1] == '/' || strstr(t, "//")) return false;
  for (; *t; t++) {
    char ch = *t;
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
          ch == '-' || ch == '/'))
      return false;
  }
  return true;
}
bool allPrintable(const char* s) {
  for (; *s; s++)
    if ((uint8_t)*s < 0x20 || *s == 0x7F) return false;
  return true;
}

// skraćuje UTF-8 tekst na granici znaka i zamenjuje kontrolne znakove razmakom
void sanitizeUtf8(char* dst, const char* src, size_t size) {
  size_t n = 0;
  while (*src && n + 1 < size) {
    uint8_t b = (uint8_t)*src;
    size_t clen = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 1;
    if (n + clen >= size) break;
    if (clen == 1 && (b < 0x20 || b == 0x7F)) {
      dst[n++] = ' ';
      src++;
      continue;
    }
    for (size_t i = 0; i < clen && *src; i++) dst[n++] = *src++;
  }
  dst[n] = '\0';
}

void getStr(JsonObjectConst o, const char* key, char* dst, size_t size, Ctx& ctx) {
  JsonVariantConst v = o[key];
  if (v.isNull()) return;
  if (!v.is<const char*>()) return ctx.fail(key, F("očekivan tekst"));
  const char* s = v.as<const char*>();
  if (strlen(s) >= size) return ctx.fail(key, F("predugačko"));
  strlcpySafe(dst, s, size);
}

// broj sa jednom decimalom -> desetinke (npr. -1.5 -> -15)
void getTenths(JsonObjectConst o, const char* key, float lo, float hi, int16_t& dst, Ctx& ctx) {
  JsonVariantConst v = o[key];
  if (v.isNull()) return;
  if (!v.is<float>()) return ctx.fail(key, F("očekivan broj"));
  float x = v.as<float>();
  if (x < lo || x > hi) return ctx.fail(key, F("vrednost van dozvoljenog opsega"));
  dst = (int16_t)lroundf(x * 10);
}

template <typename T>
void getInt(JsonObjectConst o, const char* key, long lo, long hi, T& dst, Ctx& ctx) {
  JsonVariantConst v = o[key];
  if (v.isNull()) return;
  if (!v.is<long>()) return ctx.fail(key, F("očekivan ceo broj"));
  long x = v.as<long>();
  if (x < lo || x > hi) return ctx.fail(key, F("vrednost van dozvoljenog opsega"));
  dst = (T)x;
}

void getBool(JsonObjectConst o, const char* key, bool& dst, Ctx& ctx) {
  JsonVariantConst v = o[key];
  if (v.isNull()) return;
  if (!v.is<bool>()) return ctx.fail(key, F("očekivano true/false"));
  dst = v.as<bool>();
}

}  // namespace

bool configValidate(const Config& c, char* err, size_t errLen) {
  Ctx ctx{err, errLen};
  size_t hl = strlen(c.hostname);
  if (hl == 0 || c.hostname[0] == '-' || c.hostname[hl - 1] == '-') ctx.fail("hostname", F("neispravno ime"));
  for (const char* p = c.hostname; *p; p++)
    if (!isHostChar(*p)) ctx.fail("hostname", F("dozvoljena su mala slova, cifre i '-'"));
  size_t pl = strlen(c.wifiPass);
  if (pl != 0 && (pl < 8 || pl > 63)) ctx.fail("pass", F("Wi-Fi lozinka mora imati 8-63 znaka"));
  pl = strlen(c.apPass);
  if (pl < 8 || pl > 63) ctx.fail("apPass", F("lozinka pristupne tačke mora imati 8-63 znaka"));
  if (!allPrintable(c.adminPass)) ctx.fail("adminPass", F("nedozvoljeni znakovi"));
  if (c.portalAfterFails < 1 || c.portalAfterFails > 50) ctx.fail("portalFails", F("opseg 1-50"));

  if (strlen(c.tz) < 3 || !allPrintable(c.tz) || strchr(c.tz, ' ')) ctx.fail("tz", F("neispravan POSIX TZ zapis"));
  if (!allPrintable(c.tzName)) ctx.fail("tzName", F("nedozvoljeni znakovi"));
  bool anyNtp = false;
  for (uint8_t i = 0; i < 3; i++) {
    for (const char* p = c.ntp[i]; *p; p++)
      if (!isServerChar(*p)) ctx.fail("ntp", F("neispravna adresa NTP servera"));
    if (c.ntp[i][0]) anyNtp = true;
  }
  if (!anyNtp) ctx.fail("ntp", F("potreban je bar jedan NTP server"));
  if (c.dateFormat >= DATE_FORMAT_COUNT) ctx.fail("dateFmt", F("nepoznat format"));
  if (c.animDir > ANIM_DOWN) ctx.fail("anim", F("nepoznata animacija"));
  if (c.dateDot >= DATE_DOT_COUNT) ctx.fail("dot", F("nepoznat izgled tačke"));

  if (c.hwType >= HW_TYPE_COUNT) ctx.fail("hw", F("nepoznat tip modula"));
  if (c.modules < MODULES_MIN || c.modules > MODULES_MAX) ctx.fail("modules", F("opseg 3-16"));
  if (c.timeFont < TIME_FONT_FIRST || c.timeFont > TIME_FONT_LAST) ctx.fail("timeFont", F("nepoznat font"));
  if (c.colonStyle >= COLON_STYLE_COUNT) ctx.fail("colon", F("nepoznat izgled dvotačke"));
  if (c.brightDay > 15 || c.brightNight > 15 || c.brightMax > 15) ctx.fail("osvetljaj", F("opseg 0-15"));
  if (c.nightStart >= 1440 || c.nightEnd >= 1440) ctx.fail("noć", F("neispravno vreme"));
  if (c.nightMode > NIGHT_OFF) ctx.fail("nMode", F("nepoznat režim"));
  if (c.scrollSpeed < 10 || c.scrollSpeed > 200) ctx.fail("speed", F("opseg 10-200 ms"));
  if (c.scrollPause > 10000) ctx.fail("pause", F("opseg 0-10000 ms"));
  if (c.scrollDir > SCROLL_DOWN) ctx.fail("dir", F("nepoznat smer"));
  if (c.timeSeconds < 3) ctx.fail("tSec", F("najmanje 3 s"));
  if (c.infoSeconds < 1 || c.infoSeconds > 60) ctx.fail("iSec", F("opseg 1-60 s"));
  uint8_t seen = 0;
  for (uint8_t i = 0; i < PAGE_COUNT; i++) {
    if (c.pageOrder[i] >= PAGE_COUNT) ctx.fail("order", F("nepoznata stranica"));
    else seen |= 1 << c.pageOrder[i];
  }
  if (seen != (1 << PAGE_COUNT) - 1) ctx.fail("order", F("svaka stranica mora biti navedena tačno jednom"));

  const MqttConfig& m = c.mqtt;
  for (const char* p = m.host; *p; p++)
    if (!isServerChar(*p)) ctx.fail("mqtt.host", F("neispravna adresa brokera"));
  if (!validTopic(m.base)) ctx.fail("mqtt.base", F("neispravna tema (slova, cifre, _ - /)"));
  if (!validTopic(m.discPrefix)) ctx.fail("mqtt.discPrefix", F("neispravna tema"));
  if (m.teleSeconds < 30 || m.teleSeconds > 3600) ctx.fail("mqtt.tele", F("opseg 30-3600 s"));
  if (!allPrintable(m.user) || !allPrintable(m.pass)) ctx.fail("mqtt", F("nedozvoljeni znakovi"));
  const SensorConfig& s = c.sens;
  if (s.type > 3) ctx.fail("sens.type", F("nepoznat senzor"));
  if (s.dhtPin != 16 && s.dhtPin != 5 && s.dhtPin != 4) ctx.fail("sens.dhtPin", F("dozvoljeni su D0, D1 i D2"));
  if (s.interval < 10 || s.interval > 3600) ctx.fail("sens.interval", F("opseg 10-3600 s"));
  if (s.tOffset < -100 || s.tOffset > 100) ctx.fail("sens.tOff", F("opseg -10 do +10 °C"));
  if (s.hOffset < -200 || s.hOffset > 200) ctx.fail("sens.hOff", F("opseg -20 do +20 %"));

  const WeatherConfig& w = c.owm;
  for (const char* p = w.key; *p; p++)
    if (!isalnum((uint8_t)*p)) ctx.fail("owm.key", F("API ključ sme da sadrži samo slova i cifre"));
  if (!allPrintable(w.city) || !allPrintable(w.label)) ctx.fail("owm", F("nedozvoljeni znakovi"));
  if (w.intervalMin < 10 || w.intervalMin > 180) ctx.fail("owm.interval", F("opseg 10-180 min"));
  if (w.enabled && (!w.key[0] || !w.city[0])) ctx.fail("owm", F("unesite API ključ i grad"));

  if (m.enabled) {
    if (!m.host[0]) ctx.fail("mqtt.host", F("unesite adresu brokera"));
    if (m.port == 0) ctx.fail("mqtt.port", F("opseg 1-65535"));
    // bez anonimne veze (specifikacija)
    if (!m.user[0] || !m.pass[0]) ctx.fail("mqtt", F("potrebni su korisničko ime i lozinka"));
  }
  return ctx.ok;
}

// ---------------------------------------------------------------- JSON

static void toJson(const Config& c, JsonObject o, bool secrets) {
  JsonObject net = o["net"].to<JsonObject>();
  net["hostname"] = c.hostname;
  net["ssid"] = c.wifiSsid;
  net["portalFails"] = c.portalAfterFails;
  if (secrets) {
    net["pass"] = c.wifiPass;
    net["apPass"] = c.apPass;
    net["adminPass"] = c.adminPass;
  } else {
    net["passSet"] = c.wifiPass[0] != '\0';
    net["adminPassSet"] = c.adminPass[0] != '\0';
  }

  JsonObject t = o["time"].to<JsonObject>();
  t["tzName"] = c.tzName;
  t["tz"] = c.tz;
  JsonArray ntp = t["ntp"].to<JsonArray>();
  for (uint8_t i = 0; i < 3; i++) ntp.add(c.ntp[i]);
  t["h24"] = c.h24;
  t["seconds"] = c.showSeconds;
  t["anim"] = c.animDir;
  t["animH"] = c.animHour;
  t["animM"] = c.animMin;
  t["animS"] = c.animSec;
  t["blink"] = c.blinkColon;
  t["lead0"] = c.leadingZero;
  t["dateFmt"] = c.dateFormat;
  t["dot"] = c.dateDot;
  t["dateNarrow"] = c.dateNarrow;
  t["cyr"] = c.cyrillic;

  JsonObject d = o["disp"].to<JsonObject>();
  d["hw"] = c.hwType;
  d["modules"] = c.modules;
  d["timeFont"] = c.timeFont;
  d["colon"] = c.colonStyle;
  d["bDay"] = c.brightDay;
  d["bNight"] = c.brightNight;
  d["bMax"] = c.brightMax;
  d["night"] = c.nightEnabled;
  d["nStart"] = c.nightStart;
  d["nEnd"] = c.nightEnd;
  d["nMode"] = c.nightMode;
  d["speed"] = c.scrollSpeed;
  d["pause"] = c.scrollPause;
  d["dir"] = c.scrollDir;
  d["tSec"] = c.timeSeconds;
  d["iSec"] = c.infoSeconds;
  JsonArray order = d["order"].to<JsonArray>();
  JsonArray pages = d["pages"].to<JsonArray>();
  for (uint8_t i = 0; i < PAGE_COUNT; i++) {
    order.add(c.pageOrder[i]);
    pages.add(c.pageEnabled[i]);
  }

  JsonArray msgs = o["msgs"].to<JsonArray>();
  for (uint8_t i = 0; i < MSG_COUNT; i++) {
    JsonObject m = msgs.add<JsonObject>();
    m["on"] = c.msgs[i].enabled;
    m["t"] = c.msgs[i].text;
  }

  JsonObject mq = o["mqtt"].to<JsonObject>();
  mq["on"] = c.mqtt.enabled;
  mq["host"] = c.mqtt.host;
  mq["port"] = c.mqtt.port;
  mq["user"] = c.mqtt.user;
  if (secrets) mq["pass"] = c.mqtt.pass;
  else mq["passSet"] = c.mqtt.pass[0] != '\0';
  mq["base"] = c.mqtt.base;
  mq["disc"] = c.mqtt.discovery;
  mq["discPrefix"] = c.mqtt.discPrefix;
  mq["tele"] = c.mqtt.teleSeconds;

  JsonObject se = o["sens"].to<JsonObject>();
  se["type"] = c.sens.type;
  se["dhtPin"] = c.sens.dhtPin;
  se["interval"] = c.sens.interval;
  se["tOff"] = c.sens.tOffset / 10.0f;
  se["hOff"] = c.sens.hOffset / 10.0f;
  se["showT"] = c.sens.showTemp;
  se["showH"] = c.sens.showHum;
  se["showP"] = c.sens.showPress;

  JsonObject ow = o["owm"].to<JsonObject>();
  ow["on"] = c.owm.enabled;
  if (secrets) ow["key"] = c.owm.key;
  else ow["keySet"] = c.owm.key[0] != '\0';
  ow["city"] = c.owm.city;
  ow["interval"] = c.owm.intervalMin;
  ow["label"] = c.owm.label;
  ow["showT"] = c.owm.showTemp;
  ow["showF"] = c.owm.showFeels;
  ow["showH"] = c.owm.showHum;
  ow["showP"] = c.owm.showPress;
  ow["showW"] = c.owm.showWind;
  ow["showD"] = c.owm.showDesc;
}

void configToJson(const Config& c, JsonObject o) { toJson(c, o, false); }

bool configFromJson(Config& c, JsonObjectConst o, char* err, size_t errLen) {
  Ctx ctx{err, errLen};
  err[0] = '\0';

  JsonObjectConst net = o["net"];
  if (!net.isNull()) {
    getStr(net, "hostname", c.hostname, sizeof(c.hostname), ctx);
    // Wi-Fi: promena SSID-a uvek postavlja i lozinku (otvorena mreža = prazna);
    // za isti SSID lozinka se menja samo ako je poslata neprazna.
    JsonVariantConst ssid = net["ssid"];
    JsonVariantConst pass = net["pass"];
    if (!ssid.isNull()) {
      char newSsid[sizeof(c.wifiSsid)] = "";
      getStr(net, "ssid", newSsid, sizeof(newSsid), ctx);
      if (ctx.ok && strcmp(newSsid, c.wifiSsid) != 0) {
        strlcpySafe(c.wifiSsid, newSsid, sizeof(c.wifiSsid));
        c.wifiPass[0] = '\0';
        getStr(net, "pass", c.wifiPass, sizeof(c.wifiPass), ctx);
      }
    }
    if (pass.is<const char*>() && pass.as<const char*>()[0]) getStr(net, "pass", c.wifiPass, sizeof(c.wifiPass), ctx);
    JsonVariantConst ap = net["apPass"];
    if (ap.is<const char*>() && ap.as<const char*>()[0]) getStr(net, "apPass", c.apPass, sizeof(c.apPass), ctx);
    JsonVariantConst adm = net["adminPass"];
    if (adm.is<const char*>() && adm.as<const char*>()[0]) getStr(net, "adminPass", c.adminPass, sizeof(c.adminPass), ctx);
    if (net["adminPassClear"] == true) c.adminPass[0] = '\0';
    getInt(net, "portalFails", 1, 50, c.portalAfterFails, ctx);
  }

  JsonObjectConst t = o["time"];
  if (!t.isNull()) {
    getStr(t, "tzName", c.tzName, sizeof(c.tzName), ctx);
    getStr(t, "tz", c.tz, sizeof(c.tz), ctx);
    JsonArrayConst ntp = t["ntp"];
    if (!ntp.isNull()) {
      for (uint8_t i = 0; i < 3; i++) {
        JsonVariantConst v = ntp[i];
        const char* s = v.is<const char*>() ? v.as<const char*>() : "";
        if (strlen(s) >= sizeof(c.ntp[i])) ctx.fail("ntp", F("predugačka adresa"));
        else strlcpySafe(c.ntp[i], s, sizeof(c.ntp[i]));
      }
    }
    getBool(t, "h24", c.h24, ctx);
    getBool(t, "seconds", c.showSeconds, ctx);
    getInt(t, "anim", 0, ANIM_DOWN, c.animDir, ctx);
    getBool(t, "animH", c.animHour, ctx);
    getBool(t, "animM", c.animMin, ctx);
    getBool(t, "animS", c.animSec, ctx);
    getBool(t, "blink", c.blinkColon, ctx);
    getBool(t, "lead0", c.leadingZero, ctx);
    getInt(t, "dateFmt", 0, DATE_FORMAT_COUNT - 1, c.dateFormat, ctx);
    getInt(t, "dot", 0, DATE_DOT_COUNT - 1, c.dateDot, ctx);
    getBool(t, "dateNarrow", c.dateNarrow, ctx);
    getBool(t, "cyr", c.cyrillic, ctx);
  }

  JsonObjectConst d = o["disp"];
  if (!d.isNull()) {
    getInt(d, "hw", 0, HW_TYPE_COUNT - 1, c.hwType, ctx);
    getInt(d, "modules", MODULES_MIN, MODULES_MAX, c.modules, ctx);
    getInt(d, "timeFont", TIME_FONT_FIRST, TIME_FONT_LAST, c.timeFont, ctx);
    getInt(d, "colon", 0, COLON_STYLE_COUNT - 1, c.colonStyle, ctx);
    getInt(d, "bDay", 0, 15, c.brightDay, ctx);
    getInt(d, "bNight", 0, 15, c.brightNight, ctx);
    getInt(d, "bMax", 0, 15, c.brightMax, ctx);
    getBool(d, "night", c.nightEnabled, ctx);
    getInt(d, "nStart", 0, 1439, c.nightStart, ctx);
    getInt(d, "nEnd", 0, 1439, c.nightEnd, ctx);
    getInt(d, "nMode", 0, NIGHT_OFF, c.nightMode, ctx);
    getInt(d, "speed", 10, 200, c.scrollSpeed, ctx);
    getInt(d, "pause", 0, 10000, c.scrollPause, ctx);
    getInt(d, "dir", 0, SCROLL_DOWN, c.scrollDir, ctx);
    getInt(d, "tSec", 3, 250, c.timeSeconds, ctx);
    getInt(d, "iSec", 1, 60, c.infoSeconds, ctx);
    // starija šema ima manje stranica: nove se dodaju na kraj redosleda, uključene
    JsonArrayConst order = d["order"];
    if (!order.isNull()) {
      size_t n = order.size();
      if (n == 0 || n > PAGE_COUNT) ctx.fail("order", F("pogrešan broj stranica"));
      else {
        uint8_t seen = 0;
        for (uint8_t i = 0; i < n; i++) {
          long v = order[i].is<long>() ? order[i].as<long>() : -1;
          if (v < 0 || v >= PAGE_COUNT || (seen & (1 << v))) {
            ctx.fail("order", F("nepoznata ili ponovljena stranica"));
            break;
          }
          c.pageOrder[i] = (uint8_t)v;
          seen |= 1 << v;
        }
        for (uint8_t p = 0, i = n; p < PAGE_COUNT && i < PAGE_COUNT; p++)
          if (!(seen & (1 << p))) c.pageOrder[i++] = p;
      }
    }
    JsonArrayConst pages = d["pages"];
    if (!pages.isNull()) {
      if (pages.size() == 0 || pages.size() > PAGE_COUNT) ctx.fail("pages", F("pogrešan broj stranica"));
      else
        for (uint8_t i = 0; i < PAGE_COUNT; i++) c.pageEnabled[i] = i < pages.size() ? pages[i] == true : true;
    }
  }

  JsonArrayConst msgs = o["msgs"];
  if (!msgs.isNull()) {
    if (msgs.size() > MSG_COUNT) ctx.fail("msgs", F("previše poruka"));
    for (uint8_t i = 0; i < MSG_COUNT; i++) {
      JsonObjectConst m = msgs[i];
      const char* s = m["t"].is<const char*>() ? m["t"].as<const char*>() : "";
      if (strlen(s) > MSG_MAX_LEN) ctx.fail("msgs", F("poruka je predugačka (najviše 150 bajtova)"));
      sanitizeUtf8(c.msgs[i].text, s, sizeof(c.msgs[i].text));
      c.msgs[i].enabled = m["on"] == true && c.msgs[i].text[0];
    }
  }

  JsonObjectConst mq = o["mqtt"];
  if (!mq.isNull()) {
    getBool(mq, "on", c.mqtt.enabled, ctx);
    getStr(mq, "host", c.mqtt.host, sizeof(c.mqtt.host), ctx);
    getInt(mq, "port", 1, 65535, c.mqtt.port, ctx);
    getStr(mq, "user", c.mqtt.user, sizeof(c.mqtt.user), ctx);
    JsonVariantConst mp = mq["pass"];
    if (mp.is<const char*>() && mp.as<const char*>()[0]) getStr(mq, "pass", c.mqtt.pass, sizeof(c.mqtt.pass), ctx);
    getStr(mq, "base", c.mqtt.base, sizeof(c.mqtt.base), ctx);
    getBool(mq, "disc", c.mqtt.discovery, ctx);
    getStr(mq, "discPrefix", c.mqtt.discPrefix, sizeof(c.mqtt.discPrefix), ctx);
    getInt(mq, "tele", 30, 3600, c.mqtt.teleSeconds, ctx);
  }

  JsonObjectConst se = o["sens"];
  if (!se.isNull()) {
    getInt(se, "type", 0, 3, c.sens.type, ctx);
    getInt(se, "dhtPin", 0, 16, c.sens.dhtPin, ctx);
    getInt(se, "interval", 10, 3600, c.sens.interval, ctx);
    getTenths(se, "tOff", -10, 10, c.sens.tOffset, ctx);
    getTenths(se, "hOff", -20, 20, c.sens.hOffset, ctx);
    getBool(se, "showT", c.sens.showTemp, ctx);
    getBool(se, "showH", c.sens.showHum, ctx);
    getBool(se, "showP", c.sens.showPress, ctx);
  }

  JsonObjectConst ow = o["owm"];
  if (!ow.isNull()) {
    getBool(ow, "on", c.owm.enabled, ctx);
    JsonVariantConst k = ow["key"];
    if (k.is<const char*>() && k.as<const char*>()[0]) getStr(ow, "key", c.owm.key, sizeof(c.owm.key), ctx);
    getStr(ow, "city", c.owm.city, sizeof(c.owm.city), ctx);
    getInt(ow, "interval", 10, 180, c.owm.intervalMin, ctx);
    getBool(ow, "showT", c.owm.showTemp, ctx);
    getBool(ow, "showF", c.owm.showFeels, ctx);
    getBool(ow, "showH", c.owm.showHum, ctx);
    getBool(ow, "showP", c.owm.showPress, ctx);
    getBool(ow, "showW", c.owm.showWind, ctx);
    getBool(ow, "showD", c.owm.showDesc, ctx);
    JsonVariantConst lb = ow["label"];
    if (!lb.isNull()) {
      const char* s = lb.is<const char*>() ? lb.as<const char*>() : "";
      if (strlen(s) >= sizeof(c.owm.label)) ctx.fail("owm.label", F("predugačak natpis"));
      else sanitizeUtf8(c.owm.label, s, sizeof(c.owm.label));
    }
  }

  if (!ctx.ok) return false;
  return configValidate(c, err, errLen);
}

// ---------------------------------------------------------------- flash

bool configLoad() {
  configDefaults(cfg);
  configError[0] = '\0';
  if (!LittleFS.exists(CONFIG_FILE)) {
    LOG("Konfiguracija ne postoji, koriste se podrazumevane vrednosti");
    return true;
  }
  File f = LittleFS.open(CONFIG_FILE, "r");
  if (!f) {
    strlcpySafe(configError, "ne mogu da otvorim config.json", sizeof(configError));
    return false;
  }
  JsonDocument doc;
  DeserializationError de = deserializeJson(doc, f);
  f.close();
  if (de) {
    snprintf_P(configError, sizeof(configError), PSTR("oštećen config.json (%s)"), de.c_str());
    LOG("GREŠKA: %s — rade podrazumevane vrednosti", configError);
    return false;
  }
  uint16_t schema = doc["schema"] | 0;
  if (schema == 0 || schema > CONFIG_SCHEMA) {
    snprintf_P(configError, sizeof(configError), PSTR("nepoznata verzija šeme (%u)"), schema);
    LOG("GREŠKA: %s — rade podrazumevane vrednosti", configError);
    return false;
  }
  // učitavanje ide preko privremene kopije: ako bilo šta ne valja, cfg ostaje podrazumevan
  std::unique_ptr<Config> tmp(new (std::nothrow) Config);
  if (!tmp) {
    strlcpySafe(configError, "nema memorije za učitavanje", sizeof(configError));
    return false;
  }
  configDefaults(*tmp);
  char err[80];
  if (!configFromJson(*tmp, doc.as<JsonObjectConst>(), err, sizeof(err))) {
    snprintf_P(configError, sizeof(configError), PSTR("neispravna konfiguracija: %s"), err);
    LOG("GREŠKA: %s — rade podrazumevane vrednosti", configError);
    return false;
  }
  cfg = *tmp;
  if (schema < CONFIG_SCHEMA) LOG("Konfiguracija prevedena sa šeme %u na %u (važi posle čuvanja)", schema, CONFIG_SCHEMA);
  else LOG("Konfiguracija učitana (šema %u)", schema);
  return true;
}

bool configSave(const Config& c) {
  char err[80];
  if (!configValidate(c, err, sizeof(err))) {
    LOG("Čuvanje odbijeno: %s", err);
    return false;
  }
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  root["schema"] = CONFIG_SCHEMA;
  root["fw"] = FW_VERSION;
  toJson(c, root, true);
  if (doc.overflowed()) return false;

  File f = LittleFS.open(CONFIG_TMP, "w");
  if (!f) return false;
  size_t n = serializeJson(doc, f);
  f.close();
  if (n == 0) return false;
  if (!LittleFS.rename(CONFIG_TMP, CONFIG_FILE)) {
    LittleFS.remove(CONFIG_TMP);
    return false;
  }
  configError[0] = '\0';
  LOG("Konfiguracija sačuvana (%u B)", (unsigned)n);
  return true;
}

void configFactoryReset() {
  LittleFS.remove(CONFIG_FILE);
  LittleFS.remove(CONFIG_TMP);
  configDefaults(cfg);
  configError[0] = '\0';
  LOG("Fabrička podešavanja vraćena");
}
