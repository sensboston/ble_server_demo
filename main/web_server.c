#include "web_server.h"
#include "ble_server.h"
#include "led_controller.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
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

// Log a web UI action (caller must NOT hold log_mutex)
static void web_log_action(const char *desc)
{
    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    uint16_t doff = store_data(desc);
    // Always log web actions regardless of s_log_enabled
    uint16_t idx = (log_head + log_count) % LOG_MAX_ENTRIES;
    if (log_count >= LOG_MAX_ENTRIES) {
        log_head = (log_head + 1) % LOG_MAX_ENTRIES;
    } else {
        log_count++;
    }
    time_t now;
    time(&now);
    log_entries[idx].timestamp   = (uint32_t)now;
    log_entries[idx].device_idx  = 0xFF;
    log_entries[idx].event_type  = WEB_EVT_ACTION;
    log_entries[idx].char_idx    = 0xFF;
    log_entries[idx].data_offset = doff;
    xSemaphoreGive(log_mutex);
}

void web_log_boot(void)
{
    // Compute actual boot wall-clock time: current time minus seconds since boot
    time_t boot_ts = time(NULL) - (time_t)(esp_timer_get_time() / 1000000LL);

    if (xSemaphoreTake(log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    uint16_t doff = store_data("Boot");
    uint16_t idx  = (log_head + log_count) % LOG_MAX_ENTRIES;
    if (log_count >= LOG_MAX_ENTRIES) {
        log_head = (log_head + 1) % LOG_MAX_ENTRIES;
    } else {
        log_count++;
    }
    log_entries[idx].timestamp   = (uint32_t)boot_ts;
    log_entries[idx].device_idx  = 0xFF;
    log_entries[idx].event_type  = WEB_EVT_ACTION;
    log_entries[idx].char_idx    = 0xFF;
    log_entries[idx].data_offset = doff;
    xSemaphoreGive(log_mutex);
}

// --- HTTP handlers ---

static const char *event_name(uint8_t evt)
{
    switch (evt) {
    case BLE_EVT_CONNECT:    return "CONN";
    case BLE_EVT_DISCONNECT: return "DISC";
    case BLE_EVT_READ:       return "RD";
    case BLE_EVT_WRITE:      return "WR";
    case WEB_EVT_ACTION:     return "HTTP";
    default:                 return "?";
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

// Serve main HTML page with tabbed interface
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
        "<title>ESP32 BLE Server</title>"
        "<style>"
        ":root{--bg:#1e1e1e;--bg2:#2d2d2d;--bg3:#252525;--bd:#444;--bd2:#333;"
        "--tx:#d4d4d4;--tx2:#888;--hdr:#569cd6;--th:#9cdcfe;--btn:#3a3a3a;--btnH:#4a4a4a}"
        ".light{--bg:#f5f5f5;--bg2:#e8e8e8;--bg3:#efefef;--bd:#ccc;--bd2:#ddd;"
        "--tx:#1a1a1a;--tx2:#666;--hdr:#0066b8;--th:#005fa3;--btn:#ddd;--btnH:#ccc}"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "html{height:100%}"
        "body{font-family:monospace;background:var(--bg);color:var(--tx);padding:8px;height:100%;"
        "max-width:540px;margin:0 auto;display:flex;flex-direction:column}"
        "header{display:flex;align-items:center;gap:8px;padding:6px 0;"
        "border-bottom:1px solid var(--bd);margin-bottom:8px}"
        "h1{font-size:15px;color:var(--hdr)}"
        ".tabs{display:flex;gap:2px;border-bottom:1px solid var(--bd);margin-bottom:0}"
        ".tab{background:var(--btn);color:var(--tx2);border:1px solid var(--bd2);"
        "border-bottom:none;border-radius:3px 3px 0 0;padding:5px 14px;"
        "font-family:monospace;font-size:13px;cursor:pointer;margin-bottom:-1px;position:relative}"
        ".tab.active{background:var(--bg2);color:var(--hdr);border-color:var(--bd);"
        "border-bottom-color:var(--bg2);z-index:1}"
        ".tab:hover:not(.active){background:var(--btnH)}"
        ".pane{display:none}"
        ".pane.active{display:flex;flex-direction:column;flex:1;min-height:0;"
        "background:var(--bg2);border:1px solid var(--bd);"
        "border-top:none;padding:12px;border-radius:0 0 3px 3px}"
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
        ".CONN{color:#4ec9b0}.DISC{color:#ce9178}"
        ".RD{color:#569cd6}.WR{color:#dcdcaa}.HTTP{color:#c792ea}"
        ".dt{font-size:10px;color:var(--tx2)}"
        ".lbl{font-size:11px;color:var(--tx2);margin-bottom:4px}"
        ".cur{font-size:18px;color:var(--hdr);margin-bottom:12px;min-height:24px}"
        ".inp{background:var(--bg3);color:var(--tx);border:1px solid var(--bd);border-radius:3px;"
        "padding:5px 8px;font-family:monospace;font-size:13px;width:100%;margin-bottom:8px}"
        ".row{display:flex;gap:8px;align-items:center;margin-bottom:10px}"
        ".row:last-child{margin-bottom:0}"
        ".row button{flex:1}"
        ".lbl2{flex:0 0 130px;font-size:12px;color:var(--tx2)}"
        ".anim-row{display:grid;grid-template-columns:repeat(6,1fr);gap:6px;margin-bottom:10px}"
        ".anim-row button{font-size:11px;padding:4px 2px}"
        "@media(max-width:480px){"
        "td,th{padding:3px 4px;font-size:11px}"
        ".anim-row{grid-template-columns:repeat(3,1fr)}}"
        "</style>"
        "<script>var t=localStorage.getItem('t')||'dark';"
        "if(t==='light')document.documentElement.className='light';"
        "</script>"
        "</head><body>"
        "<header>"
        "<svg width='20' height='20' viewBox='0 0 24 24' fill='none' stroke='var(--hdr)'"
        " stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round' style='flex-shrink:0'>"
        "<polyline points='6.5 6.5 17.5 17.5 12 23 12 1 17.5 6.5 6.5 17.5'/></svg>"
        "<h1>ESP32 BLE Server</h1>"
        "</header>"
        "<div class='tabs'>"
        "<button class='tab active' id='t0'>Demo</button>"
        "<button class='tab' id='t1'>Log</button>"
        "<button class='tab' id='t2'>Settings</button>"
        "</div>"
        "<div class='pane active' id='p0'>"
        "<div class='lbl'>Current NVS value:</div>"
        "<div class='cur' id='curVal'>...</div>"
        "<div class='lbl'>New value:</div>"
        "<input class='inp' id='newVal' type='text' maxlength='20' placeholder='enter value...'>"
        "<button onclick='writeVal()'>Write to NVS</button>"
        "<hr style='border:none;border-top:1px solid var(--bd);margin:12px 0'>"
        "<div class='lbl'>LED Color:</div>"
        "<canvas id='cpick' height='150'"
        " style='cursor:crosshair;border-radius:3px;border:1px solid var(--bd);"
        "margin-bottom:8px;touch-action:none;display:block;width:100%'></canvas>"
        "<button onclick='setLedColor()' style='margin-bottom:10px'>Set Color</button>"
        "<div class='lbl'>LED Animation:</div>"
        "<div class='anim-row'>"
        "<button id='btnFade'      onclick='setLedAnim(\"fade\")'>Fade</button>"
        "<button id='btnFire'      onclick='setLedAnim(\"fire\")'>Fire</button>"
        "<button id='btnRainbow'   onclick='setLedAnim(\"rainbow\")'>Rainbow</button>"
        "<button id='btnHeartbeat' onclick='setLedAnim(\"heartbeat\")'>Heartbeat</button>"
        "<button id='btnBreathe'   onclick='setLedAnim(\"breathe\")'>Breathe</button>"
        "<button id='btnMorse'     onclick='setLedAnim(\"morse\")'>Morse</button>"
        "</div>"
        "</div>"
        "<div class='pane' id='p1'>"
        "<div style='flex:1;overflow-y:auto;min-height:0'>"
        "<table><thead>"
        "<tr><th>Time</th><th>Event</th><th>Device</th><th>Data</th></tr>"
        "</thead><tbody id='tb'></tbody></table>"
        "</div>"
        "<div style='padding-top:8px;text-align:right'>"
        "<button class='danger' onclick='clr()'>Clear Log</button></div>"
        "</div>"
        "<div class='pane' id='p2'>"
        "<div class='row'><span class='lbl2'>BLE Advertising:</span><button id='bBle'>...</button></div>"
        "<div class='row'><span class='lbl2'>Event Logging:</span><button id='bLog'>...</button></div>"
        "<div class='row'><span class='lbl2'>UI Theme:</span><button onclick='tTheme()'>Toggle Theme</button></div>"
        "<div class='row'><span class='lbl2'>WiFi:</span>"
        "<button class='danger' onclick='rstWifi()'>Reset WiFi</button></div>"
        "<hr style='border:none;border-top:1px solid var(--bd);margin:10px 0'>"
        "<div class='lbl'>Morse thresholds (ms) &mdash; "
        "<a href='https://play.google.com/store/apps/details?id=com.wuangxkee.graduationproject'"
        " style='color:var(--hdr)' target='_blank'>Flash Morse Code</a>:</div>"
        "<div class='row'><span class='lbl2'>Dot / Dash:</span>"
        "<input class='inp' id='mT1' type='number' min='50' max='1500'"
        " style='flex:1;margin-bottom:0'></div>"
        "<div class='row'><span class='lbl2'>Sym / Letter:</span>"
        "<input class='inp' id='mT2' type='number' min='50' max='1500'"
        " style='flex:1;margin-bottom:0'></div>"
        "<div class='row'><span class='lbl2'>Letter / Word:</span>"
        "<input class='inp' id='mT3' type='number' min='200' max='3000'"
        " style='flex:1;margin-bottom:0'></div>"
        "<div class='row'><span class='lbl2'></span>"
        "<button onclick='saveMorse()'>Apply</button></div>"
        "</div>"
        "<script>"
        "var TT=[['t0','p0'],['t1','p1'],['t2','p2']];"
        "TT.forEach(function(pair){"
        "document.getElementById(pair[0]).onclick=function(){"
        "TT.forEach(function(p){"
        "document.getElementById(p[0]).className='tab';"
        "document.getElementById(p[1]).className='pane';});"
        "this.className='tab active';"
        "document.getElementById(pair[1]).className='pane active';"
        "localStorage.setItem('tab',pair[0]);"
        "if(pair[0]==='t0')setTimeout(resizePick,0);};});"
        "(function(){var s=localStorage.getItem('tab');"
        "if(s&&s!=='t0'){var e=document.getElementById(s);if(e)e.click();}})();"
        "var on=true,ln=true;"
        "function upBtns(){"
        "var b=document.getElementById('bBle');"
        "b.textContent='BLE '+(on?'ON':'OFF');b.className=on?'on':'off';"
        "var l=document.getElementById('bLog');"
        "l.textContent='Log '+(ln?'ON':'OFF');l.className=ln?'on':'off';}"
        "document.getElementById('bBle').onclick=function(){"
        "fetch('/ble',{method:'POST',body:on?'0':'1'})"
        ".then(r=>r.json()).then(s=>{on=s.ble;upBtns();});};"
        "document.getElementById('bLog').onclick=function(){"
        "fetch('/logging',{method:'POST',body:ln?'0':'1'})"
        ".then(r=>r.json()).then(s=>{ln=s.log;upBtns();});};"
        "function clr(){fetch('/clear',{method:'POST'}).then(upd);}"
        "function rstWifi(){"
        "if(!confirm('Reset WiFi? Device will reboot into provisioning mode.'))return;"
        "fetch('/reset-wifi',{method:'POST'}).then(function(){"
        "alert('Rebooting... Connect to AP ESP32_XXXXXX to re-provision.');});}"
        "function tTheme(){"
        "var cur=document.documentElement.className==='light';"
        "fetch('/theme',{method:'POST',body:cur?'dark':'light'})"
        ".then(r=>r.json()).then(s=>{"
        "document.documentElement.className=s.theme==='light'?'light':'';"
        "localStorage.setItem('t',s.theme);});}"
        "function loadVal(){"
        "fetch('/value').then(r=>r.json()).then(d=>{"
        "document.getElementById('curVal').textContent=d.value;});}"
        "function writeVal(){"
        "var v=document.getElementById('newVal').value;"
        "fetch('/value',{method:'POST',body:v}).then(r=>r.json()).then(d=>{"
        "document.getElementById('curVal').textContent=d.value;"
        "document.getElementById('newVal').value='';});}"
        "var ledAnims=['btnFade','btnFire','btnRainbow','btnHeartbeat','btnBreathe','btnMorse'];"
        "var ledIds={'fade':'btnFade','fire':'btnFire','rainbow':'btnRainbow',"
        "'heartbeat':'btnHeartbeat','breathe':'btnBreathe','morse':'btnMorse'};"
        "function setLedActive(id){"
        "ledAnims.forEach(function(a){document.getElementById(a).className='';});"
        "if(id)document.getElementById(id).className='on';}"
        "var _h=0,_s=1,_v=1,_dirty=false,resizePick,morseLoaded=false;"
        "function hsv2hex(h,s,v){"
        "var r,g,b,i=Math.floor(h*6),f=h*6-i,"
        "p=v*(1-s),q=v*(1-f*s),t=v*(1-(1-f)*s);"
        "switch(i%6){case 0:r=v,g=t,b=p;break;case 1:r=q,g=v,b=p;break;"
        "case 2:r=p,g=v,b=t;break;case 3:r=p,g=q,b=v;break;"
        "case 4:r=t,g=p,b=v;break;case 5:r=v,g=p,b=q;}"
        "return((r*255|0)<<16|(g*255|0)<<8|(b*255|0)).toString(16).padStart(6,'0');}"
        "function drawPick(){"
        "var cv=document.getElementById('cpick'),ctx=cv.getContext('2d');"
        "var w=cv.width,h=cv.height,hs=20,sv=h-hs;"
        "var g1=ctx.createLinearGradient(0,0,w,0);"
        "g1.addColorStop(0,'#fff');"
        "g1.addColorStop(1,'hsl('+(_h*360)+',100%,50%)');"
        "ctx.fillStyle=g1;ctx.fillRect(0,0,w,sv);"
        "var g2=ctx.createLinearGradient(0,0,0,sv);"
        "g2.addColorStop(0,'rgba(0,0,0,0)');g2.addColorStop(1,'#000');"
        "ctx.fillStyle=g2;ctx.fillRect(0,0,w,sv);"
        "var gh=ctx.createLinearGradient(0,0,w,0);"
        "[0,1/6,2/6,3/6,4/6,5/6,1].forEach(function(t){"
        "gh.addColorStop(t,'hsl('+(t*360)+',100%,50%)');});"
        "ctx.fillStyle=gh;ctx.fillRect(0,sv,w,hs);"
        "ctx.strokeStyle='rgba(255,255,255,0.9)';ctx.lineWidth=1.5;"
        "ctx.beginPath();ctx.arc(_s*w,(1-_v)*sv,6,0,Math.PI*2);ctx.stroke();"
        "ctx.beginPath();ctx.arc(_h*w,sv+hs/2,6,0,Math.PI*2);ctx.stroke();}"
        "function pickAt(x,y){"
        "_dirty=true;"
        "var cv=document.getElementById('cpick'),hs=20,sv=cv.height-hs;"
        "var r=cv.getBoundingClientRect();"
        "var cx=x-r.left,cy=y-r.top;"
        "if(cy<sv){_s=Math.max(0,Math.min(1,cx/cv.width));"
        "_v=Math.max(0,Math.min(1,1-cy/sv));}"
        "else{_h=Math.max(0,Math.min(0.9999,cx/cv.width));}"
        "drawPick();}"
        "(function(){"
        "var cv=document.getElementById('cpick');"
        "cv.addEventListener('mousedown',function(e){"
        "pickAt(e.clientX,e.clientY);"
        "function mv(e){pickAt(e.clientX,e.clientY);}"
        "function up(){document.removeEventListener('mousemove',mv);"
        "document.removeEventListener('mouseup',up);}"
        "document.addEventListener('mousemove',mv);"
        "document.addEventListener('mouseup',up);});"
        "cv.addEventListener('touchstart',function(e){"
        "e.preventDefault();pickAt(e.touches[0].clientX,e.touches[0].clientY);"
        "},{passive:false});"
        "cv.addEventListener('touchmove',function(e){"
        "e.preventDefault();pickAt(e.touches[0].clientX,e.touches[0].clientY);"
        "},{passive:false});"
        "resizePick=function(){"
        "var cv=document.getElementById('cpick');"
        "var w=cv.parentElement.getBoundingClientRect().width;"
        "if(w>0){cv.width=Math.round(w);drawPick();}}"
        ";"
        "resizePick();"
        "window.addEventListener('resize',resizePick);"
        "})();"
        "function setLedColor(){"
        "var h=hsv2hex(_h,_s,_v);"
        "_dirty=false;"
        "fetch('/led/color',{method:'POST',body:h}).then(function(){setLedActive(null);});}"
        "function setLedAnim(a){"
        "fetch('/led/anim',{method:'POST',body:a}).then(function(){setLedActive(ledIds[a]||null);});}"
        "function setColor(h){"
        "if(_dirty)return;"
        "var r=parseInt(h.substring(0,2),16)/255,"
        "g=parseInt(h.substring(2,4),16)/255,"
        "b=parseInt(h.substring(4,6),16)/255;"
        "var mx=Math.max(r,g,b),mn=Math.min(r,g,b),d=mx-mn;"
        "_v=mx;_s=mx?d/mx:0;"
        "if(!d){_h=0;}else if(mx===r){_h=((g-b)/d+6)%6/6;}"
        "else if(mx===g){_h=((b-r)/d+2)/6;}"
        "else{_h=((r-g)/d+4)/6;}"
        "drawPick();}"
        "function saveMorse(){"
        "fetch('/morse/cfg',{method:'POST',"
        "body:'t1='+document.getElementById('mT1').value"
        "+'&t2='+document.getElementById('mT2').value"
        "+'&t3='+document.getElementById('mT3').value});}"
        "function fState(){"
        "fetch('/state').then(r=>r.json()).then(s=>{"
        "on=s.ble;ln=s.log;"
        "document.documentElement.className=s.theme==='light'?'light':'';"
        "localStorage.setItem('t',s.theme);"
        "upBtns();"
        "if(s.led&&s.led.length===6){setColor(s.led);setLedActive(null);}"
        "else if(s.led)setLedActive(ledIds[s.led]||null);"
        "if(s.morse&&!morseLoaded){"
        "document.getElementById('mT1').value=s.morse.t1;"
        "document.getElementById('mT2').value=s.morse.t2;"
        "document.getElementById('mT3').value=s.morse.t3;"
        "morseLoaded=true;}});}"
        "function upd(){"
        "fetch('/log').then(r=>r.json()).then(d=>{"
        "var h='';"
        "for(var i=d.length-1;i>=0;i--){"
        "var e=d[i];"
        "h+='<tr><td><span class=\"dt\">'+e.date+'</span><br>'+e.time+'</td>'"
        "+'<td class=\"'+e.ev+'\">'+e.ev+'</td>'"
        "+'<td>'+e.dev+'</td><td>'+e.d+'</td></tr>';}"
        "document.getElementById('tb').innerHTML=h;"
        "});}"
        "fState();loadVal();upd();"
        "setInterval(function(){loadVal();upd();fState();},2000);"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

// GET /value - return current BLE characteristic value as JSON
static esp_err_t value_get_handler(httpd_req_t *req)
{
    char val[BLE_MAX_VALUE_LEN + 1];
    ble_get_value(val, sizeof(val));
    char resp[BLE_MAX_VALUE_LEN + 16];
    snprintf(resp, sizeof(resp), "{\"value\":\"%s\"}", val);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

// POST /value - set BLE characteristic value and persist to NVS
static esp_err_t value_post_handler(httpd_req_t *req)
{
    char body[BLE_MAX_VALUE_LEN + 1] = {0};
    int  recv = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv < 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[recv] = '\0';
    ble_set_value(body);
    led_ctrl_set_morse_text(body);  // keep Morse in sync if active
    char wdesc[32];
    snprintf(wdesc, sizeof(wdesc), "Val: %.24s", body);
    web_log_action(wdesc);

    char val[BLE_MAX_VALUE_LEN + 1];
    ble_get_value(val, sizeof(val));
    char resp[BLE_MAX_VALUE_LEN + 16];
    snprintf(resp, sizeof(resp), "{\"value\":\"%s\"}", val);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

// POST /led/color - body: "RRGGBB" hex
static esp_err_t led_color_handler(httpd_req_t *req)
{
    char body[8] = {0};
    int  recv = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv < 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[recv] = '\0';
    led_ctrl_apply_command(body);
    char desc[16];
    snprintf(desc, sizeof(desc), "LED #%s", body);
    web_log_action(desc);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

// POST /led/anim - body: "fade" | "fire" | "rainbow" | "off" | "heartbeat" | "breathe" | "morse"
static esp_err_t led_anim_handler(httpd_req_t *req)
{
    char body[12] = {0};
    int  recv = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv < 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[recv] = '\0';
    if (strcmp(body, "morse") == 0) {
        // Use current BLE characteristic value as Morse text
        char val[BLE_MAX_VALUE_LEN + 1];
        ble_get_value(val, sizeof(val));
        led_ctrl_set_morse_text(val);
    }
    led_ctrl_apply_command(body);
    char desc[20];
    snprintf(desc, sizeof(desc), "Anim: %s", body);
    web_log_action(desc);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

// POST /morse/cfg - body: "dot=NNN&dash=NNN&sym=NNN&char_g=NNN&word_g=NNN"
static esp_err_t morse_cfg_handler(httpd_req_t *req)
{
    char body[80] = {0};
    int recv = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv < 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[recv] = '\0';

    morse_cfg_t cfg;
    led_ctrl_get_morse_timing(&cfg);  // start from current values

    char *p;
    #define PARSE_FIELD(key, field, lo, hi) \
        p = strstr(body, key "="); \
        if (p) { \
            int v = atoi(p + sizeof(key)); \
            if (v >= (lo) && v <= (hi)) cfg.field = (uint16_t)v; \
        }

    PARSE_FIELD("t1", t1_ms,  50, 1500)
    PARSE_FIELD("t2", t2_ms,  50, 1500)
    PARSE_FIELD("t3", t3_ms, 200, 3000)
    #undef PARSE_FIELD

    led_ctrl_set_morse_timing(&cfg);
    web_log_action("Morse cfg saved");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
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

        char dev_str[20];
        if (e->device_idx == 0xFF) {
            strcpy(dev_str, "Web UI");
        } else if (e->device_idx < device_count) {
            uint8_t *a = device_addrs[e->device_idx];
            snprintf(dev_str, sizeof(dev_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     a[0], a[1], a[2], a[3], a[4], a[5]);
        } else {
            strcpy(dev_str, "unknown");
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

// Return current state as JSON {ble, log, theme, led, morse:{...}}
static esp_err_t state_handler(httpd_req_t *req)
{
    char led_cmd[12] = {0};
    led_ctrl_get_command(led_cmd, sizeof(led_cmd));
    morse_cfg_t mcfg;
    led_ctrl_get_morse_timing(&mcfg);
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"ble\":%s,\"log\":%s,\"theme\":\"%s\",\"led\":\"%s\","
             "\"morse\":{\"t1\":%u,\"t2\":%u,\"t3\":%u}}",
             s_ble_enabled ? "true" : "false",
             s_log_enabled ? "true" : "false",
             s_theme, led_cmd,
             mcfg.t1_ms, mcfg.t2_ms, mcfg.t3_ms);
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
    web_log_action(new_state ? "BLE: ON" : "BLE: OFF");

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
    web_log_action(new_state ? "Log: ON" : "Log: OFF");

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
    web_log_action("Log cleared");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

// POST - erase WiFi credentials and reboot into provisioning mode
static esp_err_t reset_wifi_handler(httpd_req_t *req)
{
    web_log_action("WiFi reset");
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
    config.max_uri_handlers  = 15;
    config.stack_size        = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    httpd_uri_t uris[] = {
        { "/",             HTTP_GET,  root_handler,       NULL },
        { "/log",          HTTP_GET,  log_handler,        NULL },
        { "/state",        HTTP_GET,  state_handler,      NULL },
        { "/value",        HTTP_GET,  value_get_handler,  NULL },
        { "/manifest.json",HTTP_GET,  manifest_handler,   NULL },
        { "/favicon.svg",  HTTP_GET,  favicon_handler,    NULL },
        { "/ble",          HTTP_POST, ble_ctrl_handler,   NULL },
        { "/logging",      HTTP_POST, log_ctrl_handler,   NULL },
        { "/theme",        HTTP_POST, theme_handler,      NULL },
        { "/clear",        HTTP_POST, clear_handler,      NULL },
        { "/reset-wifi",   HTTP_POST, reset_wifi_handler, NULL },
        { "/value",        HTTP_POST, value_post_handler, NULL },
        { "/led/color",   HTTP_POST, led_color_handler,  NULL },
        { "/led/anim",    HTTP_POST, led_anim_handler,   NULL },
        { "/morse/cfg",   HTTP_POST, morse_cfg_handler,  NULL },
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
