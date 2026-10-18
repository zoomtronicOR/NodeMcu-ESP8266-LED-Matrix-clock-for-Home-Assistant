// Senzori temperature/vlažnosti/pritiska. Aktivan je najviše jedan (cfg.sens.type).
// Drajveri su neblokirajući automati stanja (merenje se pokrene, rezultat se čita kasnije);
// izuzetak je DHT22, čiji protokol traži ~5 ms sa isključenim prekidima, jednom po intervalu.
// Odsutan ili neispravan senzor ne utiče na ostatak uređaja.
#pragma once
#include <Arduino.h>

enum SensorType : uint8_t { SENSOR_NONE = 0, SENSOR_AHT2X, SENSOR_BME280, SENSOR_DHT22, SENSOR_TYPE_COUNT };

// I²C pinovi (PIN_I2C_SDA/SCL) su u board.h, odvojeni od pinova matrice

void sensorBegin();  // tip i pin se čitaju samo ovde; promena traži restart
void sensorLoop();

uint8_t sensorActiveType();
uint8_t sensorActivePin();  // DHT22 pin sa kojim je pokrenut
const char* sensorTypeName(uint8_t type);
const char* sensorStateText();
const char* sensorModel();  // detektovan model, npr. "BMP280"

bool sensorHasHumidity();  // mogućnosti aktivnog senzora
bool sensorHasPressure();

// Poslednja validna vrednost (sa kalibracionim pomakom). false = nema vrednosti ili je prestara.
bool sensorTemperature(float& c);
bool sensorHumidity(float& rh);
bool sensorPressure(float& hpa);

int32_t sensorLastOkAgeS();  // -1 = nikad
uint32_t sensorReadCount();
uint32_t sensorErrorCount();
