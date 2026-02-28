#include "web_server.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "WEB_SERVER"

// Timestamps below this value (Jan 1 2020) are boot-relative, not wall-clock
#define NTP_SYNCED_THRESHOLD    1577836800UL

// --- Storage ---

// Known device addresses table
static uint8_t  device_addrs[LOG_MAX_DEVICES][6];
static uint8_t  device_count = 0;

// Known characteristic UUIDs table
static uint16_t char_uuids[LOG_MAX_CHARS];
static uint8_t  char_count = 0;

// Ring buffer for log entries
static log_entry_t log_entries[LOG_MAX_ENTRIES];
static uint16_t    log_head  = 0;   // Index of oldest entry
static uint16_t    log_count = 0;   // Number of valid entries

// Pool for READ/WRITE string data
static char     data_pool[LOG_DATA_POOL_SIZE];
static uint16_t data_pool_pos = 0;  // Next write position (wraps around)

static SemaphoreHandle_t log_mutex;

// --- Internal helpers (caller must hold log_mutex) ---

// Find or register a device address, return its index
static uint8_t get_device_idx(const uint8_t *bd_addr)
{
    if (!bd_addr) return 0xFF;

    for (uint8_t i = 0; i < device_count; i++) {
        if (memcmp(device_addrs[i], bd_addr, 6) == 0)
            return i;
    }
    if (device_count < LOG_MAX_DEVICES) {
        memcpy(device_addrs[device_count], bd_addr, 6);
        return device_count++;
    }
    return 0xFF; // Table full
}

// Store a string in data pool, return its offset (0xFFFF if no data)
static uint16_t store_data(const char *value)
{
    if (!value || value[0] == '\0') return 0xFFFF;

    size_t len = strlen(value) + 1; // Include null terminator
    if (len > 64) len = 64;         // Limit single entry size

    // Wrap around if not enough space at end
    if (data_pool_pos + len > LOG_DATA_POOL_SIZE)
        data_pool_pos = 0;

    uint16_t offset = data_pool_pos;
    memcpy(data_pool + offset, value, len);
    data_pool[offset + len - 1] = '\0';
    data_pool_pos += len;
    return offset;
}

// Find or register a characteristic UUID, return its index
static uint8_t find_or_add_char(uint16_t uuid)
{
    for (uint8_t i = 0; i < char_count; i++) {
        if (char_uuids[i] == uuid) return i;
    }
    if (char_count < LOG_MAX_CHARS) {
        char_uuids[char_count] = uuid;
        return char_count++;
    }
    return 0xFF;
}

// Add a log entry to ring buffer
static void log_add(uint8_t device_idx, ble_event_type_t event,
                    uint8_t char_idx, uint16_t data_offset)
{
    uint16_t idx = (log_head + log_count) % LOG_MAX_ENTRIES;

    if (log_count >= LOG_MAX_ENTRIES) {
        // Buffer full - overwrite oldest entry
        log_head = (log_head + 1) % LOG_MAX_ENTRIES;
    } else {
        log_count++;
    }

    time_t now;
    time(&now);

    log_entries[idx].timestamp   = (uint32_t)now;
    log_entries[idx].device_idx  = device_idx;
    log_entries[idx].event_type  = (uint8_t)event;
    log_entries[idx].char_idx    = char_idx;
    log_entries[idx].data_offset = data_offset;
}

// --- Public API ---

// Initialize logging mutex - must be called once in app_main before any tasks start
void web_log_init(void)
{
    log_mutex = xSemaphoreCreateMutex();
}

// Register a characteristic UUID - returns its index
uint8_t web_log_register_char(uint16_t uuid)
{
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return 0xFF;
    uint8_t idx = find_or_add_char(uuid);
    xSemaphoreGive(log_mutex);
    return idx;
}

void web_log_connect(const uint8_t *bd_addr)
{
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    uint8_t didx = get_device_idx(bd_addr);
    log_add(didx, BLE_EVT_CONNECT, 0xFF, 0xFFFF);
    xSemaphoreGive(log_mutex);
}

void web_log_disconnect(const uint8_t *bd_addr)
{
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    uint8_t didx = get_device_idx(bd_addr);
    log_add(didx, BLE_EVT_DISCONNECT, 0xFF, 0xFFFF);
    xSemaphoreGive(log_mutex);
}

void web_log_read(const uint8_t *bd_addr, uint16_t char_uuid, const char *value)
{
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    uint8_t  didx = get_device_idx(bd_addr);
    uint8_t  cidx = find_or_add_char(char_uuid);
    uint16_t doff = store_data(value);
    log_add(didx, BLE_EVT_READ, cidx, doff);
    xSemaphoreGive(log_mutex);
}

void web_log_write(const uint8_t *bd_addr, uint16_t char_uuid, const char *value)
{
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    uint8_t  didx = get_device_idx(bd_addr);
    uint8_t  cidx = find_or_add_char(char_uuid);
    uint16_t doff = store_data(value);
    log_add(didx, BLE_EVT_WRITE, cidx, doff);
    xSemaphoreGive(log_mutex);
}

// --- HTTP handlers ---

static const char *event_name(uint8_t evt)
{
    switch (evt) {
    case BLE_EVT_CONNECT:    return "CONNECT";
    case BLE_EVT_DISCONNECT: return "DISCONNECT";
    case BLE_EVT_READ:       return "READ";
    case BLE_EVT_WRITE:      return "WRITE";
    default:                 return "UNKNOWN";
    }
}

// Format timestamp into date_buf ("MM/DD/YY" or "boot") and time_buf ("HH:MM:SS")
static void format_timestamp(uint32_t ts, char *date_buf, size_t date_sz,
                             char *time_buf, size_t time_sz)
{
    if (ts >= NTP_SYNCED_THRESHOLD) {
        // Real wall-clock time - convert to local time using configured timezone
        time_t t = (time_t)ts;
        struct tm tinfo;
        localtime_r(&t, &tinfo);
        strftime(date_buf, date_sz, "%m/%d/%y", &tinfo);
        strftime(time_buf, time_sz, "%H:%M:%S", &tinfo);
    } else {
        // Boot-relative time
        snprintf(date_buf, date_sz, "boot");
        snprintf(time_buf, time_sz, "%02lu:%02lu:%02lu",
                 (unsigned long)(ts / 3600),
                 (unsigned long)((ts % 3600) / 60),
                 (unsigned long)(ts % 60));
    }
}

// Serve main HTML page
static esp_err_t root_handler(httpd_req_t *req)
{
    const char *html =
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32 BLE Monitor</title>"
        "<style>"
        "body{font-family:monospace;background:#1e1e1e;color:#d4d4d4;margin:20px}"
        "h1{color:#569cd6;border-bottom:1px solid #444;padding-bottom:8px}"
        "table{width:100%;border-collapse:collapse;margin-top:16px}"
        "th{background:#2d2d2d;color:#9cdcfe;padding:8px;text-align:left;border:1px solid #444}"
        "td{padding:6px 8px;border:1px solid #333;font-size:13px;vertical-align:top}"
        "tr:nth-child(even){background:#252525}"
        ".CONNECT{color:#4ec9b0}.DISCONNECT{color:#ce9178}"
        ".READ{color:#569cd6}.WRITE{color:#dcdcaa}"
        ".date{font-size:11px;color:#888}"
        ".time{font-size:13px}"
        ".status{font-size:12px;color:#888;margin-top:8px}"
        "</style></head><body>"
        "<h1>&#x1F4F6; ESP32 BLE Server Monitor</h1>"
        "<div class='status' id='status'>Connecting...</div>"
        "<table><thead>"
        "<tr><th>Time</th><th>Event</th><th>Device</th><th>Characteristic</th><th>Data</th></tr>"
        "</thead><tbody id='log'></tbody></table>"
        "<script>"
        "function update(){"
        "fetch('/log').then(r=>r.json()).then(data=>{"
        "var rows='';"
        "for(var i=data.length-1;i>=0;i--){"
        "var e=data[i];"
        "rows+='<tr>"
        "<td><span class=\"date\">'+e.date+'</span><br><span class=\"time\">'+e.time+'</span></td>"
        "<td class=\"'+e.ev+'\">'+e.ev+'</td>"
        "<td>'+e.dev+'</td><td>'+e.ch+'</td><td>'+e.d+'</td></tr>';}"
        "document.getElementById('log').innerHTML=rows;"
        "document.getElementById('status').innerText="
        "'Entries: '+data.length+' | Updated: '+new Date().toLocaleTimeString();});"
        "}"
        "update();setInterval(update,2000);"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

// Serve log as JSON array
static esp_err_t log_handler(httpd_req_t *req)
{
    // Snapshot log_count under mutex to safely size the buffer
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    uint16_t snapshot_count = log_count;
    xSemaphoreGive(log_mutex);

    // 160 bytes per entry (conservative upper bound), plus brackets
    size_t buf_size = (size_t)snapshot_count * 160 + 8;
    if (buf_size < 256) buf_size = 256;

    char *buf = malloc(buf_size);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Render at most snapshot_count entries to stay within the allocated buffer
    uint16_t render_count = log_count < snapshot_count ? log_count : snapshot_count;

    int pos = 0;
    buf[pos++] = '[';

    for (uint16_t i = 0; i < render_count; i++) {
        uint16_t idx = (log_head + i) % LOG_MAX_ENTRIES;
        log_entry_t *e = &log_entries[idx];

        // Format date and time strings
        char date_str[12];
        char time_str[12];
        format_timestamp(e->timestamp, date_str, sizeof(date_str),
                         time_str, sizeof(time_str));

        // Format device address
        char dev_str[20] = "unknown";
        if (e->device_idx < device_count) {
            uint8_t *a = device_addrs[e->device_idx];
            snprintf(dev_str, sizeof(dev_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     a[0], a[1], a[2], a[3], a[4], a[5]);
        }

        // Format characteristic UUID
        char char_str[12] = "-";
        if (e->char_idx < char_count)
            snprintf(char_str, sizeof(char_str), "0x%04X", char_uuids[e->char_idx]);

        // Get data string
        const char *data_str = "";
        if (e->data_offset != 0xFFFF && e->data_offset < LOG_DATA_POOL_SIZE)
            data_str = data_pool + e->data_offset;

        int written = snprintf(buf + pos, buf_size - (size_t)pos - 2,
                               "%s{\"date\":\"%s\",\"time\":\"%s\",\"ev\":\"%s\","
                               "\"dev\":\"%s\",\"ch\":\"%s\",\"d\":\"%s\"}",
                               i > 0 ? "," : "",
                               date_str, time_str,
                               event_name(e->event_type),
                               dev_str, char_str, data_str);
        if (written > 0) pos += written;
    }

    buf[pos++] = ']';
    buf[pos]   = '\0';

    xSemaphoreGive(log_mutex);

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, buf, pos);
    free(buf);
    return ret;
}

// Initialize and start HTTP web server
void web_server_start(void)
{
    // Note: log_mutex is created in web_log_init() during app_main startup

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t root = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = root_handler,
    };
    httpd_register_uri_handler(server, &root);

    httpd_uri_t log_uri = {
        .uri     = "/log",
        .method  = HTTP_GET,
        .handler = log_handler,
    };
    httpd_register_uri_handler(server, &log_uri);

    ESP_LOGI(TAG, "HTTP server started, open http://<device-ip>/ in browser");
}
