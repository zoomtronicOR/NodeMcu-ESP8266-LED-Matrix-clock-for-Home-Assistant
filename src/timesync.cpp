#include "timesync.h"

#include <coredecls.h>  // settimeofday_cb

#include "app.h"
#include "config.h"
#include "net.h"

static time_t lastSync = 0;
static uint32_t syncCount = 0;
static bool valid = false;

// posle ovoliko vremena bez uspešne sinhronizacije status prelazi u "nedostupan"
constexpr uint32_t NTP_FIRST_SYNC_TIMEOUT_MS = 60UL * 1000;  // od povezivanja na Wi-Fi
constexpr time_t NTP_STALE_S = 3 * 3600;                     // SNTP (lwIP) osvežava na 1 h

static void onTimeSet(bool fromSntp) {
  if (!fromSntp) return;
  valid = true;
  lastSync = time(nullptr);
  syncCount++;
  struct tm lt;
  localtime_r(&lastSync, &lt);
  LOG("NTP sinhronizacija #%lu: %02d.%02d.%04d. %02d:%02d:%02d", (unsigned long)syncCount, lt.tm_mday,
      lt.tm_mon + 1, lt.tm_year + 1900, lt.tm_hour, lt.tm_min, lt.tm_sec);
}

void timeApplyConfig() {
  // configTime pamti pokazivače na servere — cfg.ntp je globalan i živi dovoljno dugo
  configTime(cfg.tz, cfg.ntp[0][0] ? cfg.ntp[0] : nullptr, cfg.ntp[1][0] ? cfg.ntp[1] : nullptr,
             cfg.ntp[2][0] ? cfg.ntp[2] : nullptr);
  LOG("Vremenska zona: %s (%s)", cfg.tzName, cfg.tz);
}

void timeBegin() {
  settimeofday_cb(onTimeSet);
  timeApplyConfig();
}

bool timeValid() { return valid; }

TimeStatus timeStatus() {
  if (!valid) {
    return (netConnected() && elapsed(netConnectedSince(), NTP_FIRST_SYNC_TIMEOUT_MS)) ? TIME_UNAVAILABLE
                                                                                         : TIME_WAITING;
  }
  return (time(nullptr) - lastSync > NTP_STALE_S) ? TIME_UNAVAILABLE : TIME_SYNCED;
}

const char* timeStatusText(TimeStatus s) {
  switch (s) {
    case TIME_SYNCED: return "sinhronizovano";
    case TIME_UNAVAILABLE: return "NTP nedostupan";
    default: return "čeka sinhronizaciju";
  }
}

time_t timeLastSync() { return lastSync; }
uint32_t timeSyncCount() { return syncCount; }
