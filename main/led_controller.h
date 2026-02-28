#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "led_strip.h"

// Initialize LED controller; must be called before any other led_ctrl_* function
void led_ctrl_init(led_strip_handle_t led);

// Parse and apply a command string:
//   "RRGGBB"   - set static color (hex, e.g. "FF0080"); enters demo mode
//   "fade"     - smooth HSV hue sweep animation
//   "fire"     - warm red/orange flicker animation
//   "rainbow"  - fast hue cycle animation
//   "off"      - exit demo mode, restore BLE status indication
// Returns true if the command was recognised and applied.
bool led_ctrl_apply_command(const char *cmd);

// Notify LED controller of BLE connection state change (status mode only)
void led_ctrl_ble_connected(bool connected);

// Brief color flash for a BLE read (blue) or write (red) event (status mode only)
void led_ctrl_ble_flash(bool is_read);

// Get current LED state as a command string (null-terminated):
//   "RRGGBB" if static color, "fade"/"fire"/"rainbow" if animation, "off" if status mode
void led_ctrl_get_command(char *buf, size_t len);
