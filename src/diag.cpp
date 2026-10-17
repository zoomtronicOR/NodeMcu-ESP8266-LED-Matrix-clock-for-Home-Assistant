#include "diag.h"

#include "app.h"

static uint32_t heapMin = UINT32_MAX;
static uint32_t uptimeS = 0;
static uint32_t lastTick = 0;
static uint32_t loopMaxUs = 0;
static char resetReason[48];

void diagBegin() {
  strlcpySafe(resetReason, ESP.getResetReason().c_str(), sizeof(resetReason));
  heapMin = ESP.getFreeHeap();
  lastTick = millis();
  LOG("Pokretanje: firmware %s, razlog restarta: %s", FW_VERSION, resetReason);
}

void diagLoop(uint32_t loopUs) {
  if (loopUs > loopMaxUs) loopMaxUs = loopUs;
  uint32_t h = ESP.getFreeHeap();
  if (h < heapMin) heapMin = h;
  // uptime u sekundama, ne zavisi od prelivanja millis() posle 49 dana
  while (elapsed(lastTick, 1000)) {
    lastTick += 1000;
    uptimeS++;
  }
}

uint32_t diagHeapMin() { return heapMin; }
uint32_t diagUptimeS() { return uptimeS; }
uint32_t diagLoopMaxMs() { return loopMaxUs / 1000; }
const char* diagResetReason() { return resetReason; }
