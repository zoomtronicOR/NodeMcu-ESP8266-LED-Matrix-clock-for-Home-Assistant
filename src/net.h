// Wi-Fi: neblokirajuće povezivanje sa rastućim intervalom, pristupna tačka za podešavanje
// (captive portal), mDNS, FLASH taster.
#pragma once
#include <Arduino.h>

void netBegin();
void netLoop();
void netReconnectNow();  // posle promene Wi-Fi podataka

bool netConnected();
uint32_t netConnectedSince();  // millis() trenutka povezivanja
uint32_t netFailCount();       // neuspeli pokušaji od pokretanja
uint32_t netDisconnectCount(); // prekidi veze od pokretanja
const char* netStateText();

void portalStart(const char* reason);
bool portalActive();
const char* portalSsid();
const char* portalIp();
