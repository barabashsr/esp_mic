#pragma once

#include "esp_wifi_types.h"

typedef enum {
    WIFI_APP_MODE_OFFLINE = 0,
    WIFI_APP_MODE_STA,
    WIFI_APP_MODE_AP,
} wifi_app_mode_t;

// Scan nearby networks, try saved credentials (MRU), fall back to AP mode.
void wifi_init(void);

wifi_app_mode_t wifi_get_mode(void);
const char *wifi_get_ssid(void);
const char *wifi_get_ip(void);

// Scan for visible networks. Caller provides array; returns count filled.
uint16_t wifi_scan(wifi_ap_record_t *results, uint16_t max);

// Save credentials to NVS (MRU slot 0) and reboot to connect.
void wifi_save_and_connect(const char *ssid, const char *pass);

// Get saved SSIDs (up to 5). Returns count. ssids[] must be char[33] each.
int wifi_get_saved_ssids(char ssids[][33], int max);
