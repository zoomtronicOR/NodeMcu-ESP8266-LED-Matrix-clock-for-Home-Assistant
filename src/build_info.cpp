// Vreme builda (tools/build_info.py osvežava header pri svakom buildu).
#include "app.h"
#include "generated/build_info.h"

const char FW_BUILD[] = FW_BUILD_TIME;

// Oznaka ploče u firmware-u (pojavljuje se tačno jednom u .bin fajlu). Web OTA je traži u
// fajlu koji se šalje i odbija firmware za drugu ploču (web.cpp).
const char FW_TAG[] = "MCFW-TAG:" CHIP_NAME ";";
