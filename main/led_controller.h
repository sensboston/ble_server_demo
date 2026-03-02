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
//   "RRGGBB" if static color, animation name if active, "off" if status mode
void led_ctrl_get_command(char *buf, size_t len);

// Set the text string to transmit when Morse animation is active
void led_ctrl_set_morse_text(const char *text);

// Morse decoder thresholds — mirror the three sliders in "Flash Morse Code" app.
// Firmware derives actual on/off durations automatically with comfortable margins:
//   dot  = 60% T1,  dash = 250% T1
//   sym  = 40% T2,  char = midpoint(T2,T3),  word = 150% T3
typedef struct {
    uint16_t t1_ms;  // Dot/Dash threshold:    signal ≤ t1 → dot,  > t1 → dash
    uint16_t t2_ms;  // Sym/Letter threshold:  gap    ≤ t2 → sym,  (t2..t3] → char
    uint16_t t3_ms;  // Letter/Word threshold: gap    > t3 → word
} morse_cfg_t;

// Get / set Morse timing (set also persists to NVS)
void led_ctrl_get_morse_timing(morse_cfg_t *cfg);
void led_ctrl_set_morse_timing(const morse_cfg_t *cfg);
