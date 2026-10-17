// MQTT + Home Assistant Discovery (AsyncMqttClient — nikad ne blokira glavnu petlju).
// Potpuno opciono: kad je isključen ili broker ne radi, ostatak uređaja radi normalno.
//
// Teme (<base> = cfg.mqtt.base):
//   <base>/status              online/offline (retained, LWT)
//   <base>/state               {"brightness","mode","on"} (retained, na promenu)
//   <base>/telemetry           RSSI, uptime, heap, NTP... (na cfg.mqtt.teleSeconds)
//   <base>/command/brightness  0-15 (ručno, do sledeće smene dan/noć)
//   <base>/command/message     tekst ili {"text":"...","seconds":60}; prazno = ukloni
//   <base>/command/display_mode normal | off | dnd
//   <base>/command/page        time | date | weekday | messages
//   <base>/command/test, <base>/command/restart
#pragma once
#include <Arduino.h>

#include "config.h"

void mqttBegin();
void mqttLoop();
void mqttApplyConfig();  // posle promene MQTT podešavanja
bool mqttConfigChanged(const MqttConfig& a, const MqttConfig& b);

bool mqttConnected();
const char* mqttStateText();
const char* mqttLastError();
uint32_t mqttConnectCount();
uint32_t mqttFailCount();
time_t mqttLastConnect();  // 0 = nikad
