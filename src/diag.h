// Dijagnostika: heap (trenutni/najniži), uptime bez prelivanja, razlog restarta, najduža petlja.
#pragma once
#include <Arduino.h>

void diagBegin();
void diagLoop(uint32_t loopUs);  // poziva se jednom po prolazu glavne petlje

uint32_t diagHeapMin();
uint32_t diagUptimeS();
uint32_t diagLoopMaxMs();
const char* diagResetReason();
