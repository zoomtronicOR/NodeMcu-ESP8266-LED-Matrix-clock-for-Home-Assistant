#include "platform.h"

#include <LittleFS.h>

#include "app.h"

#if defined(ESP8266)

void platformSerialBegin(uint32_t baud) { Serial.begin(baud); }

bool platformFsBegin() { return LittleFS.begin(); }  // ESP8266 sam formatira praznu particiju

uint32_t chipId() { return ESP.getChipId(); }
uint32_t heapMaxBlock() { return ESP.getMaxFreeBlockSize(); }
uint8_t heapFragmentation() { return ESP.getHeapFragmentation(); }
uint32_t flashSize() { return ESP.getFlashChipRealSize(); }
uint32_t sketchSize() { return ESP.getSketchSize(); }
uint32_t sketchFreeSpace() { return ESP.getFreeSketchSpace(); }

bool fsInfo(uint32_t& used, uint32_t& total) {
  FSInfo fsi;
  if (!LittleFS.info(fsi)) return false;
  used = fsi.usedBytes;
  total = fsi.totalBytes;
  return true;
}

void resetReasonText(char* buf, size_t len) { strlcpySafe(buf, ESP.getResetReason().c_str(), len); }

void wifiSetHostname(const char* name) { WiFi.hostname(name); }
bool wifiScanIsOpen(int i) { return WiFi.encryptionType(i) == ENC_TYPE_NONE; }

#else  // ESP32

#include <esp_system.h>

void platformSerialBegin(uint32_t baud) {
#if ARDUINO_USB_CDC_ON_BOOT
  // USB CDC: kad je kabl u računaru, a terminal nije otvoren, upis ne sme da koči petlju
  Serial.setTxBufferSize(2048);
  Serial.setTxTimeoutMs(10);
#endif
  Serial.begin(baud);
}

bool platformFsBegin() { return LittleFS.begin(true); }

uint32_t chipId() {
  uint64_t mac = ESP.getEfuseMac();  // bajt 0 MAC adrese je u najnižih 8 bita
  return (uint32_t)(((mac >> 24) & 0xFF) << 16 | ((mac >> 32) & 0xFF) << 8 | ((mac >> 40) & 0xFF));
}
uint32_t heapMaxBlock() { return ESP.getMaxAllocHeap(); }
uint8_t heapFragmentation() {
  uint32_t f = ESP.getFreeHeap();
  return f ? (uint8_t)(100 - (uint64_t)ESP.getMaxAllocHeap() * 100 / f) : 0;
}
uint32_t flashSize() { return ESP.getFlashChipSize(); }
uint32_t sketchSize() {
  // ESP32 računa veličinu proverom celog image-a (sporo) — jednom po pokretanju
  static uint32_t size = ESP.getSketchSize();
  return size;
}
// isto značenje kao na ESP8266: slobodno u prostoru za firmware (OTA particija = app particija)
uint32_t sketchFreeSpace() {
  uint32_t part = ESP.getFreeSketchSpace(), used = sketchSize();
  return part > used ? part - used : 0;
}

bool fsInfo(uint32_t& used, uint32_t& total) {
  total = LittleFS.totalBytes();
  used = LittleFS.usedBytes();
  return total > 0;
}

void resetReasonText(char* buf, size_t len) {
  const char* r;
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: r = "Power On"; break;
    case ESP_RST_EXT: r = "External System"; break;
    case ESP_RST_SW: r = "Software/System restart"; break;
    case ESP_RST_PANIC: r = "Exception"; break;
    case ESP_RST_INT_WDT: r = "Interrupt Watchdog"; break;
    case ESP_RST_TASK_WDT: r = "Task Watchdog"; break;
    case ESP_RST_WDT: r = "Hardware Watchdog"; break;
    case ESP_RST_DEEPSLEEP: r = "Deep-Sleep Wake"; break;
    case ESP_RST_BROWNOUT: r = "Brownout (napajanje)"; break;
    case ESP_RST_USB: r = "USB"; break;
    case ESP_RST_JTAG: r = "JTAG"; break;
    default: r = "Unknown"; break;
  }
  strlcpySafe(buf, r, len);
}

void wifiSetHostname(const char* name) { WiFi.setHostname(name); }
bool wifiScanIsOpen(int i) { return WiFi.encryptionType(i) == WIFI_AUTH_OPEN; }

#endif
