#pragma once

#include "esp_err.h"
#include <stdint.h>

#define SD_MOUNT_POINT "/sdcard"

// Initialize SPI bus and mount FAT filesystem on SD card.
esp_err_t sdcard_init(void);

// Get free space on SD card in bytes.
uint64_t sdcard_free_bytes(void);
