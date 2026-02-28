#pragma once

#include "esp_err.h"
#include <stdint.h>

// Initialize SSD1306 128×32 over I2C (pins/address defined in config.h).
// Returns ESP_OK on success; logs a warning and returns error code otherwise.
esp_err_t oled_init(void);

// Fill the entire display with zeros (all pixels off).
void oled_clear(void);

// Print ASCII text starting at the given page (0–3) and column (0–127).
// Each character occupies 6 pixel-columns (5 glyph + 1 gap).
// Text is clipped at the right edge of the display.
void oled_puts(uint8_t page, uint8_t col, const char *text);

// Print ASCII text at 2× scale: each character is 8 columns wide and spans
// two pages (page and page+1).  'page' must be 0, 1, or 2.
// Up to 16 characters fit per row on a 128-pixel wide display.
// Glyphs are row-doubled from the built-in 5×7 font (no extra font data needed).
void oled_puts_large(uint8_t page, uint8_t col, const char *text);

// Write text to display line 0, 1, or 2 and refresh the screen.
// Uses a cross-page vertical layout: 3 text lines with 3px gaps between them,
// centred in the 32px height with 1px top margin and 4px bottom margin.
// Thread-safe: protected by an internal FreeRTOS mutex.
void oled_set_line(uint8_t line, const char *text);
