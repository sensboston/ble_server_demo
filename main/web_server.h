#pragma once

#include <stdint.h>
#include "config.h"

// BLE event types
typedef enum {
    BLE_EVT_CONNECT    = 0,
    BLE_EVT_DISCONNECT = 1,
    BLE_EVT_READ       = 2,
    BLE_EVT_WRITE      = 3,
} ble_event_type_t;

// Compact log entry - 9 bytes per record
// timestamp holds Unix time (seconds) when NTP is synced,
// or seconds since boot otherwise (distinguishable: boot values < Jan 1 2020)
typedef struct __attribute__((packed)) {
    uint32_t timestamp;     // Unix time if synced, seconds since boot otherwise
    uint8_t  device_idx;    // Index into device address table (0xFF = unknown)
    uint8_t  event_type;    // ble_event_type_t
    uint8_t  char_idx;      // Index into characteristic UUID table (0xFF = unknown)
    uint16_t data_offset;   // Offset into data pool (0xFFFF = no data)
} log_entry_t;

// Initialize logging mutex - call once in app_main before any tasks start
void web_log_init(void);

// Initialize and start HTTP web server
void web_server_start(void);

// Log a BLE connection event
void web_log_connect(const uint8_t *bd_addr);

// Log a BLE disconnection event
void web_log_disconnect(const uint8_t *bd_addr);

// Log a BLE read event
void web_log_read(const uint8_t *bd_addr, uint16_t char_uuid, const char *value);

// Log a BLE write event
void web_log_write(const uint8_t *bd_addr, uint16_t char_uuid, const char *value);

// Register a characteristic UUID - returns its index
uint8_t web_log_register_char(uint16_t uuid);
