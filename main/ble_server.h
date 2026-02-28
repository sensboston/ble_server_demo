#pragma once

#include <stdbool.h>
#include "led_strip.h"

// Initialize and start BLE GATT server
void ble_server_start(led_strip_handle_t led);

// Enable or disable BLE advertising; disconnects active client when disabling
void ble_set_enabled(bool enabled);

// Returns current BLE enabled state
bool ble_is_enabled(void);

// Callback type for WiFi reset triggered from BLE write
typedef void (*ble_wifi_reset_cb_t)(void);

// Register callback invoked when the reset characteristic receives "1"
void ble_set_wifi_reset_cb(ble_wifi_reset_cb_t cb);