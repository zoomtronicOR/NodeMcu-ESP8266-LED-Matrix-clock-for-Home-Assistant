#include "sensors.h"

#include <Wire.h>

#include "app.h"
#include "config.h"
#include "platform.h"

constexpr uint8_t AHT_ADDR = 0x38;
constexpr uint32_t STALE_MIN_MS = 10UL * 60 * 1000;  // posle ovoliko se vrednost više ne prikazuje
constexpr uint8_t ERRORS_FOR_RECOVERY = 3;           // uzastopne greške -> oporavak I²C magistrale

enum Phase : uint8_t { PH_IDLE, PH_AHT_INIT, PH_MEASURING, PH_DHT_START };

static uint8_t type = SENSOR_NONE;
static uint8_t dhtPin = DHT_PIN_DEFAULT;
static bool present = false;
static char model[12] = "";
static Phase phase = PH_IDLE;
static uint32_t phaseT0 = 0, waitMs = 0;
static uint32_t lastStart = 0;
static bool started = false;

static float tRaw = 0, hRaw = 0, pRaw = 0;
static bool hasH = false, hasP = false;
static bool everOk = false;
static uint32_t lastOkMs = 0;
static uint32_t reads = 0, errors = 0;
static uint8_t consecutiveErr = 0;

// ---------------------------------------------------------------- I²C pomoćne

static bool i2cWrite(uint8_t addr, const uint8_t* d, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(d, n);
  return Wire.endTransmission() == 0;
}

static bool i2cRead(uint8_t addr, uint8_t* d, size_t n) {
  if (Wire.requestFrom(addr, (uint8_t)n) != n) return false;
  for (size_t i = 0; i < n; i++) d[i] = Wire.read();
  return true;
}

static bool regRead(uint8_t addr, uint8_t reg, uint8_t* d, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  return i2cRead(addr, d, n);
}

static bool regWrite(uint8_t addr, uint8_t reg, uint8_t v) {
  uint8_t d[2] = {reg, v};
  return i2cWrite(addr, d, 2);
}

static bool i2cProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// Oslobađa magistralu ako je neki uređaj zadržao SDA na niskom nivou (9 taktova + STOP)
static void i2cRecover() {
  LOG("Senzor: oporavak I²C magistrale");
#if defined(ESP32)
  Wire.end();  // ESP32: pinovi moraju biti oslobođeni od I²C periferije
#endif
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, OUTPUT_OPEN_DRAIN);
  for (uint8_t i = 0; i < 9 && digitalRead(PIN_I2C_SDA) == LOW; i++) {
    digitalWrite(PIN_I2C_SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(PIN_I2C_SCL, HIGH);
    delayMicroseconds(5);
  }
  pinMode(PIN_I2C_SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(PIN_I2C_SDA, LOW);
  delayMicroseconds(5);
  digitalWrite(PIN_I2C_SCL, HIGH);
  delayMicroseconds(5);
  digitalWrite(PIN_I2C_SDA, HIGH);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);
}

// ---------------------------------------------------------------- AHT20 / AHT21

static uint8_t ahtCrc(const uint8_t* d, size_t n) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (uint8_t b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

static bool ahtDetect() {
  if (!i2cProbe(AHT_ADDR)) return false;
  strlcpySafe(model, "AHT2x", sizeof(model));
  hasH = true;
  hasP = false;
  return true;
}

// vraća čekanje u ms; 0 = greška
static uint32_t ahtStart() {
  uint8_t st;
  if (!i2cRead(AHT_ADDR, &st, 1)) return 0;
  if (!(st & 0x08)) {  // nije kalibrisan: inicijalizacija, merenje u sledećem koraku
    static const uint8_t init[] = {0xBE, 0x08, 0x00};
    if (!i2cWrite(AHT_ADDR, init, sizeof(init))) return 0;
    phase = PH_AHT_INIT;
    return 20;
  }
  static const uint8_t trig[] = {0xAC, 0x33, 0x00};
  if (!i2cWrite(AHT_ADDR, trig, sizeof(trig))) return 0;
  phase = PH_MEASURING;
  return 100;  // merenje traje ~80 ms
}

static bool ahtFinish() {
  uint8_t d[7];
  if (!i2cRead(AHT_ADDR, d, sizeof(d))) return false;
  if (d[0] & 0x80) return false;  // još meri
  if (ahtCrc(d, 6) != d[6]) return false;
  uint32_t hr = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
  uint32_t tr = ((uint32_t)(d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];
  hRaw = hr * 100.0f / 1048576.0f;
  tRaw = tr * 200.0f / 1048576.0f - 50.0f;
  return true;
}

// ---------------------------------------------------------------- BME280 / BMP280

static uint8_t bmeAddr = 0x76;
static struct {
  uint16_t T1;
  int16_t T2, T3;
  uint16_t P1;
  int16_t P2, P3, P4, P5, P6, P7, P8, P9;
  uint8_t H1, H3;
  int16_t H2, H4, H5;
  int8_t H6;
} cal;

static bool bmeDetect() {
  const uint8_t addrs[] = {0x76, 0x77};
  for (uint8_t a : addrs) {
    uint8_t id;
    if (!regRead(a, 0xD0, &id, 1)) continue;
    if (id != 0x60 && id != 0x58) continue;
    bmeAddr = a;
    hasH = id == 0x60;
    hasP = true;
    strlcpySafe(model, hasH ? "BME280" : "BMP280", sizeof(model));
    uint8_t c[26];
    if (!regRead(a, 0x88, c, 26)) return false;
    cal.T1 = c[0] | (c[1] << 8);
    cal.T2 = (int16_t)(c[2] | (c[3] << 8));
    cal.T3 = (int16_t)(c[4] | (c[5] << 8));
    cal.P1 = c[6] | (c[7] << 8);
    int16_t* p = &cal.P2;
    for (uint8_t i = 0; i < 8; i++) p[i] = (int16_t)(c[8 + i * 2] | (c[9 + i * 2] << 8));
    cal.H1 = c[25];
    if (hasH) {
      uint8_t h[7];
      if (!regRead(a, 0xE1, h, 7)) return false;
      cal.H2 = (int16_t)(h[0] | (h[1] << 8));
      cal.H3 = h[2];
      cal.H4 = (int16_t)(((int16_t)(int8_t)h[3] * 16) | (h[4] & 0x0F));
      cal.H5 = (int16_t)(((int16_t)(int8_t)h[5] * 16) | (h[4] >> 4));
      cal.H6 = (int8_t)h[6];
    }
    regWrite(a, 0xF5, 0x00);  // bez filtera
    return true;
  }
  return false;
}

static uint32_t bmeStart() {
  if (hasH && !regWrite(bmeAddr, 0xF2, 0x01)) return 0;  // vlažnost x1 (mora pre ctrl_meas)
  if (!regWrite(bmeAddr, 0xF4, 0x25)) return 0;          // T x1, P x1, forced režim
  phase = PH_MEASURING;
  return 50;
}

static bool bmeFinish() {
  uint8_t st;
  if (!regRead(bmeAddr, 0xF3, &st, 1) || (st & 0x08)) return false;  // još meri
  uint8_t d[8];
  if (!regRead(bmeAddr, 0xF7, d, hasH ? 8 : 6)) return false;
  int32_t adcP = ((int32_t)d[0] << 12) | ((int32_t)d[1] << 4) | (d[2] >> 4);
  int32_t adcT = ((int32_t)d[3] << 12) | ((int32_t)d[4] << 4) | (d[5] >> 4);
  if (adcT == 0x80000) return false;  // merenje preskočeno

  // kompenzacija prema Bosch datasheet-u (celobrojna)
  int32_t v1 = ((((adcT >> 3) - ((int32_t)cal.T1 << 1))) * cal.T2) >> 11;
  int32_t v2 = (((((adcT >> 4) - (int32_t)cal.T1) * ((adcT >> 4) - (int32_t)cal.T1)) >> 12) * cal.T3) >> 14;
  int32_t tFine = v1 + v2;
  tRaw = ((tFine * 5 + 128) >> 8) / 100.0f;

  int64_t a = (int64_t)tFine - 128000;
  int64_t b = a * a * cal.P6;
  b += (a * cal.P5) << 17;
  b += (int64_t)cal.P4 << 35;
  a = ((a * a * cal.P3) >> 8) + ((a * cal.P2) << 12);
  a = ((((int64_t)1) << 47) + a) * cal.P1 >> 33;
  if (a == 0) return false;
  int64_t pr = 1048576 - adcP;
  pr = (((pr << 31) - b) * 3125) / a;
  a = ((int64_t)cal.P9 * (pr >> 13) * (pr >> 13)) >> 25;
  b = ((int64_t)cal.P8 * pr) >> 19;
  pr = ((pr + a + b) >> 8) + ((int64_t)cal.P7 << 4);
  pRaw = (uint32_t)pr / 25600.0f;  // Q24.8 Pa -> hPa

  if (hasH) {
    int32_t adcH = ((int32_t)d[6] << 8) | d[7];
    int32_t x = tFine - 76800;
    x = (((((adcH << 14) - ((int32_t)cal.H4 << 20) - ((int32_t)cal.H5 * x)) + 16384) >> 15) *
         (((((((x * cal.H6) >> 10) * (((x * (int32_t)cal.H3) >> 11) + 32768)) >> 10) + 2097152) * cal.H2 + 8192) >>
          14));
    x -= (((((x >> 15) * (x >> 15)) >> 7) * (int32_t)cal.H1) >> 4);
    x = x < 0 ? 0 : (x > 419430400 ? 419430400 : x);
    hRaw = (x >> 12) / 1024.0f;
  }
  return true;
}

// ---------------------------------------------------------------- DHT22

// trajanje nivoa u µs; UINT32_MAX = isteklo
static uint32_t IRAM_ATTR dhtPulse(uint8_t level) {
  uint32_t start = ESP.getCycleCount();
  const uint32_t limit = 1000 * (F_CPU / 1000000);  // 1 ms
  while (digitalRead(dhtPin) == level) {
    if (ESP.getCycleCount() - start > limit) return UINT32_MAX;
  }
  return (ESP.getCycleCount() - start) / (F_CPU / 1000000);
}

static uint32_t dhtStart() {
  pinMode(dhtPin, OUTPUT);
  digitalWrite(dhtPin, LOW);  // start signal, najmanje 1 ms
  phase = PH_DHT_START;
  return 3;
}

static bool dhtFinish() {
  uint8_t d[5] = {0};
  bool ok = true;
  {
    InterruptLock lock;  // ~5 ms: protokol je vremenski osetljiv
    digitalWrite(dhtPin, HIGH);
    pinMode(dhtPin, INPUT_PULLUP);
    delayMicroseconds(40);
    if (dhtPulse(HIGH) == UINT32_MAX || dhtPulse(LOW) == UINT32_MAX || dhtPulse(HIGH) == UINT32_MAX) ok = false;
    for (uint8_t i = 0; ok && i < 40; i++) {
      uint32_t lo = dhtPulse(LOW), hi = dhtPulse(HIGH);
      if (lo == UINT32_MAX || hi == UINT32_MAX) ok = false;
      else if (hi > lo) d[i / 8] |= 0x80 >> (i % 8);
    }
  }
  if (!ok) return false;
  if ((uint8_t)(d[0] + d[1] + d[2] + d[3]) != d[4]) return false;
  hRaw = ((d[0] << 8) | d[1]) / 10.0f;
  float t = (((d[2] & 0x7F) << 8) | d[3]) / 10.0f;
  tRaw = (d[2] & 0x80) ? -t : t;
  if (!present) {
    present = true;
    strlcpySafe(model, "DHT22", sizeof(model));
  }
  return true;
}

// ---------------------------------------------------------------- zajednički tok

static bool detect() {
  switch (type) {
    case SENSOR_AHT2X: present = ahtDetect(); break;
    case SENSOR_BME280: present = bmeDetect(); break;
    default: present = false; break;
  }
  return present;
}

static void recordResult(bool ok) {
  phase = PH_IDLE;
  if (ok) {
    bool inRange = tRaw >= -40 && tRaw <= 85 && (!hasH || (hRaw >= 0 && hRaw <= 100)) &&
                   (!hasP || (pRaw >= 300 && pRaw <= 1100));
    if (!inRange) {
      LOG("Senzor: vrednost van opsega odbačena (%.1f °C, %.1f %%, %.1f hPa)", tRaw, hRaw, pRaw);
      ok = false;
    }
  }
  if (ok) {
    reads++;
    consecutiveErr = 0;
    if (!everOk) LOG("Senzor: prvo očitavanje %s: %.1f °C, %.1f %%, %.1f hPa", model, tRaw, hRaw, pRaw);
    everOk = true;
    lastOkMs = millis();
    return;
  }
  errors++;
  if (consecutiveErr < 255) consecutiveErr++;
  if (type != SENSOR_DHT22 && consecutiveErr % ERRORS_FOR_RECOVERY == 0) {
    i2cRecover();
    present = false;  // ponovna detekcija u sledećem ciklusu
  }
}

static void startMeasurement() {
  lastStart = millis();
  started = true;
  if (type != SENSOR_DHT22 && !present && !detect()) {
    // nije pronađen: ne računa se kao greška očitavanja, pokušava se ponovo u sledećem intervalu
    return;
  }
  uint32_t w = 0;
  switch (type) {
    case SENSOR_AHT2X: w = ahtStart(); break;
    case SENSOR_BME280: w = bmeStart(); break;
    case SENSOR_DHT22: w = dhtStart(); break;
  }
  if (!w) return recordResult(false);
  phaseT0 = millis();
  waitMs = w;
}

void sensorBegin() {
  type = cfg.sens.type < SENSOR_TYPE_COUNT ? cfg.sens.type : SENSOR_NONE;
  dhtPin = cfg.sens.dhtPin;
  if (type == SENSOR_NONE) return;
  if (type == SENSOR_DHT22) {
    hasH = true;
    hasP = false;
    pinMode(dhtPin, INPUT_PULLUP);  // linija miruje na visokom nivou
    LOG("Senzor: DHT22 na GPIO%u", dhtPin);
  } else {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(100000);
    // nominalne mogućnosti dok senzor ne bude pronađen (Discovery, UI)
    hasH = true;
    hasP = type == SENSOR_BME280;
    if (detect()) LOG("Senzor: pronađen %s", model);
    else LOG("Senzor: %s nije pronađen na I²C (" I2C_WIRING ")", sensorTypeName(type));
  }
}

void sensorLoop() {
  if (type == SENSOR_NONE) return;
  switch (phase) {
    case PH_IDLE:
      // prvo merenje 2 s posle pokretanja (DHT22 traži vreme za stabilizaciju), zatim po intervalu
      if (started ? elapsed(lastStart, cfg.sens.interval * 1000UL) : millis() > 2000) startMeasurement();
      break;
    case PH_AHT_INIT:
      if (elapsed(phaseT0, waitMs)) {
        uint32_t w = ahtStart();
        if (!w) recordResult(false);
        else {
          phaseT0 = millis();
          waitMs = w;
        }
      }
      break;
    case PH_DHT_START:
      if (elapsed(phaseT0, waitMs)) recordResult(dhtFinish());
      break;
    case PH_MEASURING:
      if (elapsed(phaseT0, waitMs)) recordResult(type == SENSOR_AHT2X ? ahtFinish() : bmeFinish());
      break;
  }
}

// ---------------------------------------------------------------- javni API

uint8_t sensorActiveType() { return type; }
uint8_t sensorActivePin() { return dhtPin; }

const char* sensorTypeName(uint8_t t) {
  switch (t) {
    case SENSOR_AHT2X: return "AHT20/AHT21";
    case SENSOR_BME280: return "BME280/BMP280";
    case SENSOR_DHT22: return "DHT22";
    default: return "nijedan";
  }
}

const char* sensorStateText() {
  if (type == SENSOR_NONE) return "nije izabran";
  if (!everOk) {
    if (type == SENSOR_DHT22) return consecutiveErr ? "nema odziva" : "čeka prvo očitavanje";
    return present ? "čeka prvo očitavanje" : "nije pronađen";
  }
  if (consecutiveErr >= ERRORS_FOR_RECOVERY) return "greška očitavanja (prikazuje se poslednja vrednost)";
  return "radi";
}

const char* sensorModel() { return model; }
bool sensorHasHumidity() { return type != SENSOR_NONE && hasH; }
bool sensorHasPressure() { return type != SENSOR_NONE && hasP; }

static bool fresh() {
  if (!everOk) return false;
  uint32_t limit = max(STALE_MIN_MS, (uint32_t)(cfg.sens.interval * 5000UL));
  return !elapsed(lastOkMs, limit);
}

bool sensorTemperature(float& c) {
  if (!fresh()) return false;
  c = tRaw + cfg.sens.tOffset / 10.0f;
  return true;
}

bool sensorHumidity(float& rh) {
  if (!fresh() || !hasH) return false;
  rh = hRaw + cfg.sens.hOffset / 10.0f;
  rh = rh < 0 ? 0 : (rh > 100 ? 100 : rh);
  return true;
}

bool sensorPressure(float& hpa) {
  if (!fresh() || !hasP) return false;
  hpa = pRaw;
  return true;
}

int32_t sensorLastOkAgeS() { return everOk ? (int32_t)((millis() - lastOkMs) / 1000) : -1; }
uint32_t sensorReadCount() { return reads; }
uint32_t sensorErrorCount() { return errors; }
