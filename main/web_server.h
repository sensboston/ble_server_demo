#pragma once

#include <stdint.h>

// Maximum number of unique BLE devices to track
#define LOG_MAX_DEVICES     8
// Maximum number of known GATT characteristics
#define LOG_MAX_CHARS       16
// Maximum number of log entries in ring buffer
#define LOG_MAX_ENTRIES     4096
// Size of data string pool for READ/WRITE values (bytes)
#define LOG_DATA_POOL_SIZE  4096

// BLE event types (2 bits used)
typedef enum {
    BLE_EVT_CONNECT    = 0,
    BLE_EVT_DISCONNECT = 1,
    BLE_EVT_READ       = 2,
    BLE_EVT_WRITE      = 3,
} ble_event_type_t;

// Compact log entry - 7 bytes per record
typedef struct __attribute__((packed)) {
    uint32_t timestamp_ms;  // Milliseconds since boot
    uint8_t  device_idx;    // Index into device address table (0xFF = unknown)
    uint8_t  event_type;    // ble_event_type_t
    uint8_t  char_idx;      // Index into characteristic UUID table (0xFF = unknown)
    uint16_t data_offset;   // Offset into data pool (0xFFFF = no data)
} log_entry_t;

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