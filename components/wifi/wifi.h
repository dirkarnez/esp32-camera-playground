#pragma once

// #include <stdbool.h>
// #include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_sta_join_blocking(const char *ssid, const char *pass);

#ifdef __cplusplus
}
#endif
