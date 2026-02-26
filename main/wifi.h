#pragma once

#include "esp_err.h"

// Initialize WiFi in station mode and block until connected or failed.
// Returns ESP_OK on successful connection.
esp_err_t wifi_init_sta(void);
