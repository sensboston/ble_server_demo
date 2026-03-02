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
#define BLE_LED_CHAR_UUID       0xFF03  // R/W characteristic: "RRGGBB" or "fade"/"fire"/"rainbow"/"off"
#define BLE_MAX_VALUE_LEN       20
#define BLE_LED_CMD_MAX_LEN     12      // longest command: "heartbeat" = 9 chars

// --- Morse decoder thresholds (match "Flash Morse Code" app slider values) ---
// App algorithm:  signal ≤ T1 → dot,  signal > T1 → dash
//                 gap    ≤ T2 → sym,  T2 < gap ≤ T3 → char,  gap > T3 → word
// Firmware derives actual on/off durations automatically with comfortable margins.
// Defaults tuned for comfortable reception.
#define MORSE_DEFAULT_T1_MS  120   // Dot/Dash threshold   (app slider 1)
#define MORSE_DEFAULT_T2_MS  220   // Sym/Letter threshold (app slider 2)
#define MORSE_DEFAULT_T3_MS  350   // Letter/Word threshold(app slider 3)

#define BLE_DEFAULT_VALUE       "hello"

// --- OLED SSD1306 (128×32, I2C) ---
#define OLED_SDA_GPIO           5
#define OLED_SCL_GPIO           6
#define OLED_I2C_ADDR           0x3C
#define OLED_I2C_FREQ_HZ        400000

// --- WS2812 LED ---
#define LED_GPIO                8
#define LED_BRIGHTNESS          32      // 0-255; used for BLE status indication
#define LED_DEMO_BRIGHTNESS     80      // brightness ceiling for color/animation demo
#define LED_FLASH_DURATION_MS   300

// --- FreeRTOS task stack sizes ---
#define BLE_TASK_STACK          4096
#define WIFI_TASK_STACK         6144
#define LED_ANIM_TASK_STACK     4096

// --- Web log ring buffer ---
#define LOG_MAX_DEVICES         8
#define LOG_MAX_CHARS           16
#define LOG_MAX_ENTRIES         256     // 4096 entries * 200 B/entry = ~800 KB malloc, exceeds heap
#define LOG_DATA_POOL_SIZE      4096
