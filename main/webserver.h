#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// Callback type for recording commands from WebSocket clients
typedef void (*webserver_cmd_cb_t)(const char *cmd);

// Start the HTTP server with WebSocket support.
// cmd_cb is called when a WebSocket text command is received.
esp_err_t webserver_start(webserver_cmd_cb_t cmd_cb);

// Broadcast binary audio data (16-bit PCM) to all connected WebSocket clients.
void webserver_broadcast_audio(const int16_t *samples, size_t num_samples);

// Get whether any WebSocket clients are connected.
bool webserver_has_clients(void);
