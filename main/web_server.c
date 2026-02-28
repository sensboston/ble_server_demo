#include "web_server.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "WEB_SERVER"

// Timestamps below this value (Jan 1 2020) are boot-relative, not wall-clock
#define NTP_SYNCED_THRESHOLD    1577836800UL

// NVS namespace for web UI config persistence
#define CFG_NVS_NS              "web_cfg"

// --- Persistent config state ---

static bool s_ble_enabled = true;
static bool s_log_enabled = true;
static char s_theme[8]    = "dark";

static web_ble_ctrl_cb_t  s_ble_ctrl_cb   = NULL;
static web_wifi_reset_cb_t s_wifi_reset_cb = NULL;

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

// --- NVS config helpers ---

static void web_cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    size_t sz = sizeof(s_theme);
    nvs_get_str(h, "theme", s_theme, &sz);

    uint8_t v;
    if (nvs_get_u8(h, "ble_en", &v) == ESP_OK) s_ble_enabled = (v != 0);
    if (nvs_get_u8(h, "log_en", &v) == ESP_OK) s_log_enabled = (v != 0);

    nvs_close(h);
}

static void cfg_save_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

static void cfg_save_u8(const char *key, uint8_t val)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

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

// Add a log entry to ring buffer; skipped if logging is disabled
static void log_add(uint8_t device_idx, ble_event_type_t event,
                    uint8_t char_idx, uint16_t data_offset)
{
    if (!s_log_enabled) return;

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

void web_log_init(void)
{
    log_mutex = xSemaphoreCreateMutex();
    web_cfg_load(); // Load persisted settings before tasks start
}

void web_set_ble_ctrl_cb(web_ble_ctrl_cb_t cb)
{
    s_ble_ctrl_cb = cb;
}

void web_set_wifi_reset_cb(web_wifi_reset_cb_t cb)
{
    s_wifi_reset_cb = cb;
}

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
        "<meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'>"
        "<meta name='apple-mobile-web-app-capable' content='yes'>"
        "<meta name='apple-mobile-web-app-status-bar-style' content='black-translucent'>"
        "<meta name='apple-mobile-web-app-title' content='BLE Monitor'>"
        "<meta name='theme-color' content='#1e1e1e'>"
        "<link rel='manifest' href='/manifest.json'>"
        "<link rel='icon' type='image/svg+xml' href='/favicon.svg'>"
        "<title>BLE Monitor</title>"
        "<style>"
        ":root{--bg:#1e1e1e;--bg2:#2d2d2d;--bg3:#252525;--bd:#444;--bd2:#333;"
        "--tx:#d4d4d4;--tx2:#888;--hdr:#569cd6;--th:#9cdcfe;--btn:#3a3a3a;--btnH:#4a4a4a}"
        ".light{--bg:#f5f5f5;--bg2:#e8e8e8;--bg3:#efefef;--bd:#ccc;--bd2:#ddd;"
        "--tx:#1a1a1a;--tx2:#666;--hdr:#0066b8;--th:#005fa3;--btn:#ddd;--btnH:#ccc}"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{font-family:monospace;background:var(--bg);color:var(--tx);padding:8px;min-height:100vh}"
        "header{display:flex;align-items:center;gap:6px;flex-wrap:wrap;padding:6px 0;"
        "border-bottom:1px solid var(--bd);margin-bottom:8px}"
        "button{background:var(--btn);color:var(--tx);border:1px solid var(--bd);"
        "border-radius:3px;padding:4px 10px;font-family:monospace;font-size:12px;cursor:pointer}"
        "button:hover{background:var(--btnH)}"
        "button.on{color:#4ec9b0;border-color:#4ec9b0}"
        "button.off{color:#ce9178;border-color:#ce9178}"
        "button.danger{color:#f48771;border-color:#f48771}"
        "table{width:100%;border-collapse:collapse}"
        "th{background:var(--bg2);color:var(--th);padding:6px 8px;text-align:left;"
        "border:1px solid var(--bd);white-space:nowrap}"
        "td{padding:4px 6px;border:1px solid var(--bd2);font-size:12px;vertical-align:top}"
        "tr:nth-child(even){background:var(--bg3)}"
        ".CONNECT{color:#4ec9b0}.DISCONNECT{color:#ce9178}"
        ".READ{color:#569cd6}.WRITE{color:#dcdcaa}"
        ".dt{font-size:10px;color:var(--tx2)}"
        "@media(max-width:480px){td,th{padding:3px 4px;font-size:11px}}"
        "</style>"
        "<script>var t=localStorage.getItem('t')||'dark';"
        "if(t==='light')document.documentElement.className='light';"
        "</script>"
        "</head><body>"
        "<header>"
        "<svg width='18' height='18' viewBox='0 0 24 24' fill='none' stroke='var(--hdr)'"
        " stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round' style='flex-shrink:0'>"
        "<polyline points='6.5 6.5 17.5 17.5 12 23 12 1 17.5 6.5 6.5 17.5'/></svg>"
        "<button id='bBle' onclick='tBle()'>...</button>"
        "<button id='bLog' onclick='tLog()'>...</button>"
        "<button onclick='clr()'>Clear</button>"
        "<button onclick='tTheme()'>Theme</button>"
        "<button class='danger' onclick='rstWifi()'>Reset WiFi</button>"
        "</header>"
        "<table><thead>"
        "<tr><th>Time</th><th>Event</th><th>Device</th><th>Char</th><th>Data</th></tr>"
        "</thead><tbody id='tb'></tbody></table>"
        "<script>"
        "var on=true,ln=true;"
        "function upBtns(){"
        "var b=document.getElementById('bBle');"
        "b.textContent='BLE '+(on?'ON':'OFF');b.className=on?'on':'off';"
        "var l=document.getElementById('bLog');"
        "l.textContent='Log '+(ln?'ON':'OFF');l.className=ln?'on':'off';}"
        "function fState(){"
        "fetch('/state').then(r=>r.json()).then(s=>{"
        "on=s.ble;ln=s.log;"
        "document.documentElement.className=s.theme==='light'?'light':'';"
        "localStorage.setItem('t',s.theme);"
        "upBtns();});}"
        "function tBle(){"
        "fetch('/ble',{method:'POST',body:on?'0':'1'})"
        ".then(r=>r.json()).then(s=>{on=s.ble;upBtns();});}"
        "function tLog(){"
        "fetch('/logging',{method:'POST',body:ln?'0':'1'})"
        ".then(r=>r.json()).then(s=>{ln=s.log;upBtns();});}"
        "function clr(){fetch('/clear',{method:'POST'}).then(upd);}"
        "function rstWifi(){"
        "if(!confirm('Reset WiFi? Device will reboot into provisioning mode.'))return;"
        "fetch('/reset-wifi',{method:'POST'}).then(()=>{"
        "alert('Rebooting... Connect to AP ESP32_XXXXXX to re-provision.');});}"
        "function tTheme(){"
        "var cur=document.documentElement.className==='light';"
        "fetch('/theme',{method:'POST',body:cur?'dark':'light'})"
        ".then(r=>r.json()).then(s=>{"
        "document.documentElement.className=s.theme==='light'?'light':'';"
        "localStorage.setItem('t',s.theme);});}"
        "function upd(){"
        "fetch('/log').then(r=>r.json()).then(d=>{"
        "var h='';"
        "for(var i=d.length-1;i>=0;i--){"
        "var e=d[i];"
        "h+='<tr><td><span class=\"dt\">'+e.date+'</span><br>'+e.time+'</td>'"
        "+'<td class=\"'+e.ev+'\">'+e.ev+'</td>'"
        "+'<td>'+e.dev+'</td><td>'+e.ch+'</td><td>'+e.d+'</td></tr>';}"
        "document.getElementById('tb').innerHTML=h;"
        "});}"
        "fState();upd();setInterval(upd,2000);"
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

    // 200 bytes per entry (conservative upper bound), plus brackets
    size_t buf_size = (size_t)snapshot_count * 200 + 8;
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

        char date_str[12];
        char time_str[12];
        format_timestamp(e->timestamp, date_str, sizeof(date_str),
                         time_str, sizeof(time_str));

        char dev_str[20] = "unknown";
        if (e->device_idx < device_count) {
            uint8_t *a = device_addrs[e->device_idx];
            snprintf(dev_str, sizeof(dev_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     a[0], a[1], a[2], a[3], a[4], a[5]);
        }

        char char_str[12] = "-";
        if (e->char_idx < char_count)
            snprintf(char_str, sizeof(char_str), "0x%04X", char_uuids[e->char_idx]);

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

// Return current state as JSON {ble, log, theme}
static esp_err_t state_handler(httpd_req_t *req)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ble\":%s,\"log\":%s,\"theme\":\"%s\"}",
             s_ble_enabled ? "true" : "false",
             s_log_enabled ? "true" : "false",
             s_theme);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

// POST body: "1" or "0" - toggle BLE advertising on/off
static esp_err_t ble_ctrl_handler(httpd_req_t *req)
{
    char body[4] = {0};
    if (httpd_req_recv(req, body, sizeof(body) - 1) <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    bool new_state = (body[0] == '1');
    s_ble_enabled = new_state;
    cfg_save_u8("ble_en", new_state ? 1 : 0);
    if (s_ble_ctrl_cb) s_ble_ctrl_cb(new_state);

    char resp[32];
    snprintf(resp, sizeof(resp), "{\"ble\":%s}", new_state ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

// POST body: "1" or "0" - toggle event logging on/off
static esp_err_t log_ctrl_handler(httpd_req_t *req)
{
    char body[4] = {0};
    if (httpd_req_recv(req, body, sizeof(body) - 1) <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    bool new_state = (body[0] == '1');
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        s_log_enabled = new_state;
        xSemaphoreGive(log_mutex);
    }
    cfg_save_u8("log_en", new_state ? 1 : 0);

    char resp[32];
    snprintf(resp, sizeof(resp), "{\"log\":%s}", new_state ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

// POST body: "dark" or "light" - set UI theme
static esp_err_t theme_handler(httpd_req_t *req)
{
    char body[8] = {0};
    if (httpd_req_recv(req, body, sizeof(body) - 1) <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    if (strncmp(body, "light", 5) == 0)
        strncpy(s_theme, "light", sizeof(s_theme));
    else
        strncpy(s_theme, "dark", sizeof(s_theme));
    cfg_save_str("theme", s_theme);

    char resp[32];
    snprintf(resp, sizeof(resp), "{\"theme\":\"%s\"}", s_theme);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

// POST - clear all log entries and lookup tables
static esp_err_t clear_handler(httpd_req_t *req)
{
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        log_head      = 0;
        log_count     = 0;
        data_pool_pos = 0;
        device_count  = 0;
        char_count    = 0;
        xSemaphoreGive(log_mutex);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

// POST - erase WiFi credentials and reboot into provisioning mode
static esp_err_t reset_wifi_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    if (s_wifi_reset_cb) s_wifi_reset_cb();
    return ESP_OK;
}

// Serve SVG favicon (Bluetooth symbol)
static esp_err_t favicon_handler(httpd_req_t *req)
{
    const char *svg =
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none'"
        " stroke='%23569cd6' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'>"
        "<polyline points='6.5 6.5 17.5 17.5 12 23 12 1 17.5 6.5 6.5 17.5'/></svg>";
    httpd_resp_set_type(req, "image/svg+xml");
    return httpd_resp_send(req, svg, HTTPD_RESP_USE_STRLEN);
}

// Serve PWA web app manifest
static esp_err_t manifest_handler(httpd_req_t *req)
{
    const char *manifest =
        "{\"name\":\"BLE Monitor\",\"short_name\":\"BLE Mon\","
        "\"start_url\":\"/\",\"display\":\"standalone\","
        "\"background_color\":\"#1e1e1e\",\"theme_color\":\"#1e1e1e\"}";
    httpd_resp_set_type(req, "application/manifest+json");
    return httpd_resp_send(req, manifest, HTTPD_RESP_USE_STRLEN);
}

// Initialize and start HTTP web server
void web_server_start(void)
{
    // Apply persisted BLE disabled state (BLE stack is up by the time WiFi connects)
    if (!s_ble_enabled && s_ble_ctrl_cb)
        s_ble_ctrl_cb(false);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable  = true;
    config.max_uri_handlers  = 10;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t uris[] = {
        { "/",             HTTP_GET,  root_handler,     NULL },
        { "/log",          HTTP_GET,  log_handler,      NULL },
        { "/state",        HTTP_GET,  state_handler,    NULL },
        { "/manifest.json",HTTP_GET,  manifest_handler, NULL },
        { "/favicon.svg",  HTTP_GET,  favicon_handler,  NULL },
        { "/ble",          HTTP_POST, ble_ctrl_handler, NULL },
        { "/logging",      HTTP_POST, log_ctrl_handler, NULL },
        { "/theme",        HTTP_POST, theme_handler,    NULL },
        { "/clear",        HTTP_POST, clear_handler,     NULL },
        { "/reset-wifi",   HTTP_POST, reset_wifi_handler,NULL },
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++)
        httpd_register_uri_handler(server, &uris[i]);

    esp_netif_ip_info_t ip_info = {0};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
        ESP_LOGI(TAG, "HTTP server started, open http://" IPSTR "/", IP2STR(&ip_info.ip));
    else
        ESP_LOGI(TAG, "HTTP server started");
}
