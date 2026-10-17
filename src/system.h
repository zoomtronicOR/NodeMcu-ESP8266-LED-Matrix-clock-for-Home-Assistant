// Sistemske akcije zajedničke za web UI i serijsku konzolu.
#pragma once
#include <Arduino.h>

#include "config.h"

void systemLoop();
void systemRestart(uint32_t delayMs = 800);  // restart posle kratke pauze (da odgovor stigne)
void systemFactoryReset();                    // briše konfiguraciju i restartuje

// Primena nove konfiguracije (već validirane). save=true upisuje u flash.
// Vraća false ako upis nije uspeo (konfiguracija je ipak primenjena).
bool systemApplyConfig(const Config& next, bool save);

bool systemConfigDirty();    // primenjeno, ali nije sačuvano
bool systemRestartNeeded();  // promena koja deluje tek posle restarta
bool systemRestartPending(); // restart je zakazan
