#pragma once

#include "esp_err.h"

// Brings up the station interface and blocks until an IP is acquired or
// APP_WIFI_MAX_RETRY attempts have failed.
esp_err_t wifi_manager_connect_blocking(void);
