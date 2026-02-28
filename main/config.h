#pragma once

// --- Location & Time ---
#define ZIP_CODE                "02451"
#define TIMEZONE                "EST5EDT,M3.2.0,M11.1.0"

// --- NTP ---
#define NTP_SERVER              "pool.ntp.org"

// --- BLE Server ---
#define BLE_DEVICE_NAME         "ESP32_BLE_SERVER"
#define BLE_SERVICE_UUID        0x00FF
#define BLE_CHAR_UUID           0xFF01  // R/W characteristic: persistent string value
#define BLE_RESET_CHAR_UUID     0xFF02  // W-only characteristic: write "1" to reset WiFi
#define BLE_MAX_VALUE_LEN       20
#define BLE_DEFAULT_VALUE       "hello"

// --- WS2812 LED ---
#define LED_GPIO                8
#define LED_BRIGHTNESS          32
#define LED_FLASH_DURATION_MS   300

// --- FreeRTOS task stack sizes ---
#define BLE_TASK_STACK          4096
#define WIFI_TASK_STACK         6144

// --- Web log ring buffer ---
#define LOG_MAX_DEVICES         8
#define LOG_MAX_CHARS           16
#define LOG_MAX_ENTRIES         4096
#define LOG_DATA_POOL_SIZE      4096
