#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// Initialize and start BLE GATT server
void ble_server_start(void);

// Enable or disable BLE advertising; disconnects active client when disabling
void ble_set_enabled(bool enabled);

// Returns current BLE enabled state
bool ble_is_enabled(void);

// Read the current cached characteristic value into buf (null-terminated)
void ble_get_value(char *buf, size_t len);

// Update the cached characteristic value and persist it to NVS
esp_err_t ble_set_value(const char *val);