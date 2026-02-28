#include "wifi_manager.h"
#include "oled_display.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/socket.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define TAG "WIFI_MANAGER"

#define NVS_WIFI_PREV_NS  "wifi_prev"
#define NVS_WIFI_PREV_KEY "ssid"

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_events;
static bool               s_provisioning = false;
static httpd_handle_t     s_prov_server  = NULL;

// --- WiFi event handler ---

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            // Only auto-connect in normal (post-provisioning) mode
            if (!s_provisioning)
                esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_provisioning)
                xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
            else
                esp_wifi_connect(); // Auto-reconnect in normal operation
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "Client connected to provisioning AP");
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        char ip_line[22];
        snprintf(ip_line, sizeof(ip_line), "IP:" IPSTR, IP2STR(&event->ip_info.ip));
        oled_set_line(2, ip_line);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

// --- DNS server: hijacks all queries → 192.168.4.1 (captive portal) ---

static void dns_server_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(NULL);
        return;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in srv = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS server ready");

    uint8_t buf[256], resp[300];
    struct sockaddr_in cli;
    socklen_t cli_len = sizeof(cli);

    while (1) {
        int len = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                           (struct sockaddr *)&cli, &cli_len);
        if (len < 12) continue;

        // Echo header, mark as response, set ANCOUNT=1
        memcpy(resp, buf, len);
        resp[2]  = 0x81; // QR=1, AA=1
        resp[3]  = 0x80; // RA=1, RCODE=0
        resp[7]  = 1;    // ANCOUNT = 1
        resp[8]  = 0; resp[9]  = 0; // NSCOUNT = 0
        resp[10] = 0; resp[11] = 0; // ARCOUNT = 0

        // Append A-record answer: TTL=0 prevents OS from caching hijacked results
        int pos = len;
        resp[pos++] = 0xC0; resp[pos++] = 0x0C; // name: pointer to question
        resp[pos++] = 0x00; resp[pos++] = 0x01; // Type: A
        resp[pos++] = 0x00; resp[pos++] = 0x01; // Class: IN
        resp[pos++] = 0x00; resp[pos++] = 0x00; // TTL (high)
        resp[pos++] = 0x00; resp[pos++] = 0x00; // TTL = 0 (no caching)
        resp[pos++] = 0x00; resp[pos++] = 0x04; // RDLENGTH = 4
        resp[pos++] = 192;  resp[pos++] = 168;
        resp[pos++] = 4;    resp[pos++] = 1;    // 192.168.4.1

        sendto(sock, resp, pos, 0, (struct sockaddr *)&cli, cli_len);
    }

    close(sock);
    vTaskDelete(NULL);
}

// --- TCP 443 fast-reject: makes Android HTTPS probe fail in milliseconds ---
// Android runs HTTP and HTTPS probes concurrently. Without a listener on 443,
// ESP32's lwIP may silently drop SYN packets, causing the HTTPS probe to time
// out at SOCKET_TIMEOUT_MS (10 s). By accepting and immediately RST-ing the
// connection (SO_LINGER l_linger=0), we force ECONNRESET in < 5 ms, so the
// full probe cycle completes in ~1-2 s instead of 10+ s.
static void tcp443_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) { vTaskDelete(NULL); return; }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in srv = {
        .sin_family      = AF_INET,
        .sin_port        = htons(443),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&srv, sizeof(srv)) < 0 ||
        listen(sock, 4) < 0) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "TCP 443 reject listener ready");

    while (1) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int client = accept(sock, (struct sockaddr *)&cli, &cli_len);
        if (client >= 0) {
            // l_linger=0 causes RST on close instead of graceful FIN
            struct linger lg = { .l_onoff = 1, .l_linger = 0 };
            setsockopt(client, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
            close(client);
        }
    }
    close(sock);
    vTaskDelete(NULL);
}

// --- Provisioning HTTP server ---

// Decode a URL-encoded string (application/x-www-form-urlencoded)
static void url_decode(const char *src, char *dst, size_t dst_len)
{
    size_t pos = 0;
    while (*src && pos < dst_len - 1) {
        if (*src == '%' &&
            isxdigit((unsigned char)src[1]) &&
            isxdigit((unsigned char)src[2])) {
            char hex[3] = {src[1], src[2], '\0'};
            dst[pos++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[pos++] = ' ';
            src++;
        } else {
            dst[pos++] = *src++;
        }
    }
    dst[pos] = '\0';
}

// Provisioning page - dark theme, matches main web UI style
static const char PROV_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>WiFi Setup</title>"
    "<style>"
    "body{font-family:monospace;background:#1e1e1e;color:#d4d4d4;"
    "display:flex;flex-direction:column;align-items:center;padding:24px;min-height:100vh}"
    "h2{color:#569cd6;margin-bottom:20px}"
    "form{width:100%;max-width:320px}"
    "label{font-size:12px;color:#9cdcfe;display:block;margin-bottom:4px}"
    "select,input[type=password],input[type=text]{display:block;width:100%;padding:8px;"
    "margin-bottom:14px;background:#2d2d2d;color:#d4d4d4;"
    "border:1px solid #444;border-radius:3px;"
    "font-family:monospace;font-size:14px;box-sizing:border-box}"
    "button{width:100%;padding:10px;background:#0e639c;color:#fff;"
    "border:none;border-radius:3px;font-size:14px;cursor:pointer}"
    "button:hover{background:#1177bb}button:disabled{opacity:.5;cursor:default}"
    ".pw{display:flex;gap:6px;margin-bottom:14px}"
    ".pw input{flex:1;margin-bottom:0}"
    ".eye{width:42px;flex:none;padding:0;background:#2d2d2d;"
    "border:1px solid #444;color:#9cdcfe;"
    "display:flex;align-items:center;justify-content:center}"
    ".eye:hover{background:#3d3d3d}.eye.on{border-color:#4ec9b0}"
    ".icon{display:block;pointer-events:none}"
    "#st{margin-top:16px;font-size:13px;min-height:20px;text-align:center}"
    ".ok{color:#4ec9b0}.err{color:#f48771}"
    "</style></head><body>"
    "<h2>&#x1F4F6; WiFi Setup</h2>"
    "<form id='f'>"
    "<label>Network</label>"
    "<select id='ssid'><option value=''>Scanning...</option></select>"
    "<label>Password</label>"
    "<div class='pw'>"
    "<input type='password' id='pass' autocomplete='current-password'"
    " placeholder='(leave blank if open)'>"
    "<button type='button' id='eye' class='eye' onclick='tgl()'"
    " aria-label='Show password' aria-pressed='false'>"
    "<svg class='icon icon-eye' width='20' height='20' viewBox='0 0 24 24'"
    " aria-hidden='true' style='display:none'>"
    "<path d='M1 12s4-7 11-7 11 7 11 7-4 7-11 7S1 12 1 12z'"
    " fill='none' stroke='currentColor' stroke-width='2'/>"
    "<circle cx='12' cy='12' r='3' fill='none' stroke='currentColor' stroke-width='2'/>"
    "</svg>"
    "<svg class='icon icon-eye-off' width='20' height='20' viewBox='0 0 24 24'"
    " aria-hidden='true'>"
    "<path d='M1 12s4-7 11-7 11 7 11 7-4 7-11 7S1 12 1 12z'"
    " fill='none' stroke='currentColor' stroke-width='2'/>"
    "<circle cx='12' cy='12' r='3' fill='none' stroke='currentColor' stroke-width='2'/>"
    "<path d='M3 3l18 18' fill='none' stroke='currentColor'"
    " stroke-width='2' stroke-linecap='round'/>"
    "</svg>"
    "</button>"
    "</div>"
    "<button id='btn' type='submit'>Connect</button>"
    "</form>"
    "<div id='st'></div>"
    "<script>"
    "function tgl(){"
    "var i=document.getElementById('pass'),e=document.getElementById('eye');"
    "var s=i.type==='password';"
    "i.type=s?'text':'password';"
    "e.querySelector('.icon-eye').style.display=s?'':'none';"
    "e.querySelector('.icon-eye-off').style.display=s?'none':'';"
    "e.setAttribute('aria-pressed',s);"
    "e.setAttribute('aria-label',s?'Hide password':'Show password');"
    "e.classList.toggle('on',s);i.focus();}"
    "fetch('/scan').then(r=>r.json()).then(d=>{"
    "var s=document.getElementById('ssid');"
    "s.innerHTML=d.ssids.map(n=>'<option>'+n+'</option>').join('');"
    "if(d.prev){for(var i=0;i<s.options.length;i++)"
    "{if(s.options[i].value===d.prev){s.selectedIndex=i;break;}}}"
    "});"
    "document.getElementById('f').onsubmit=function(e){"
    "e.preventDefault();"
    "var btn=document.getElementById('btn'),st=document.getElementById('st');"
    "btn.disabled=true;st.textContent='Connecting...';st.className='';"
    "var b=new URLSearchParams();"
    "b.append('ssid',document.getElementById('ssid').value);"
    "b.append('pass',document.getElementById('pass').value);"
    "fetch('/connect',{method:'POST',body:b.toString(),"
    "headers:{'Content-Type':'application/x-www-form-urlencoded'}})"
    ".then(r=>r.json()).then(r=>{"
    "st.textContent=r.msg;st.className=r.ok?'ok':'err';"
    "if(!r.ok)btn.disabled=false;"
    "if(r.ok)try{window.close();}catch(e){}"
    "});"
    "};"
    "</script></body></html>";

// GET /scan - return JSON array of visible SSIDs
static esp_err_t scan_handler(httpd_req_t *req)
{
    esp_wifi_scan_start(NULL, true); // Blocking scan (~2s)

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20;

    wifi_ap_record_t *aps = malloc(ap_count * sizeof(wifi_ap_record_t));
    if (!aps) { httpd_resp_send_500(req); return ESP_FAIL; }
    esp_wifi_scan_get_ap_records(&ap_count, aps);

    // Read previously used SSID (saved before last WiFi reset)
    char prev_ssid[33] = {0};
    nvs_handle_t nvs_h;
    if (nvs_open(NVS_WIFI_PREV_NS, NVS_READONLY, &nvs_h) == ESP_OK) {
        size_t len = sizeof(prev_ssid);
        nvs_get_str(nvs_h, NVS_WIFI_PREV_KEY, prev_ssid, &len);
        nvs_close(nvs_h);
    }

    // Build JSON: {"ssids":["net1","net2"],"prev":"MyNetwork"}
    char buf[1100];
    int pos = snprintf(buf, sizeof(buf), "{\"ssids\":[");
    bool first = true;
    for (int i = 0; i < ap_count; i++) {
        if (aps[i].ssid[0] == '\0') continue; // Skip hidden networks
        if (!first) buf[pos++] = ',';
        pos += snprintf(buf + pos, sizeof(buf) - pos - 20,
                        "\"%s\"", (char *)aps[i].ssid);
        first = false;
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"prev\":\"%s\"}", prev_ssid);
    free(aps);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

// POST /connect - try to connect with submitted credentials
static esp_err_t connect_handler(httpd_req_t *req)
{
    char body[256] = {0};
    if (httpd_req_recv(req, body, sizeof(body) - 1) <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Extract and URL-decode SSID and password
    char raw_ssid[64] = {0}, raw_pass[128] = {0};
    httpd_query_key_value(body, "ssid", raw_ssid, sizeof(raw_ssid));
    httpd_query_key_value(body, "pass", raw_pass, sizeof(raw_pass));

    char ssid[33] = {0}, pass[65] = {0};
    url_decode(raw_ssid, ssid, sizeof(ssid));
    url_decode(raw_pass, pass, sizeof(pass));

    if (ssid[0] == '\0') {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"msg\":\"No network selected.\"}",
                               HTTPD_RESP_USE_STRLEN);
    }

    ESP_LOGI(TAG, "Provisioning: SSID=%s", ssid);

    // Clear stale bits, set credentials, attempt connection
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid,     ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();

    // Wait for connection result (max 12s)
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(12000));

    if (bits & WIFI_CONNECTED_BIT) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":true,\"msg\":\"Connected! Rebooting...\"}",
                        HTTPD_RESP_USE_STRLEN);
        vTaskDelay(pdMS_TO_TICKS(800)); // Let browser receive the response
        esp_restart();
    }

    // Connection failed - reset STA for next attempt
    esp_wifi_disconnect();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req,
                           "{\"ok\":false,\"msg\":\"Connection failed. Check password.\"}",
                           HTTPD_RESP_USE_STRLEN);
}

// GET / and OS probe URLs (iOS /hotspot-detect.html, Android /generate_204, etc.)
// → serve portal HTML directly as HTTP 200 so the OS mini-browser renders it.
// Location header on 200 is advisory: Android NetworkMonitor captures it as the
// portal URL to open in the browser, avoiding an extra round-trip redirect.
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PROV_HTML, sizeof(PROV_HTML) - 1);
}

// GET /connecttest.txt (Windows NCSI) → redirect to logout.net opens the real browser
static esp_err_t ncsi_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://logout.net");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, NULL, 0);
}

// GET /* - 302 redirect to provisioning page (catch-all for anything not matched above)
static esp_err_t captive_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, NULL, 0);
}

static void start_provisioning(void)
{
    // Build AP name from last 3 bytes of MAC
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char ap_name[16];
    snprintf(ap_name, sizeof(ap_name), "ESP32_%02X%02X%02X", mac[3], mac[4], mac[5]);

    char ap_line[22];
    snprintf(ap_line, sizeof(ap_line), "AP: %s", ap_name);
    oled_set_line(0, "WiFi Setup");
    oled_set_line(1, ap_line);
    oled_set_line(2, "192.168.4.1");

    // Configure open SoftAP
    wifi_config_t ap_cfg = {
        .ap = {
            .max_connection = 4,
            .authmode       = WIFI_AUTH_OPEN,
        }
    };
    strncpy((char *)ap_cfg.ap.ssid, ap_name, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(ap_name);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    // Start DNS and HTTP servers BEFORE esp_wifi_start() - both bind to INADDR_ANY
    // so sockets are ready the instant the AP accepts the first client connection.
    // Android fires captive portal probes immediately on association; if servers
    // start after esp_wifi_start() there is a race that causes the probe to fail.
    xTaskCreate(dns_server_task, "dns_srv", 3072, NULL, 5, NULL);
    xTaskCreate(tcp443_task,    "tcp443",  2048, NULL, 5, NULL);

    // Provisioning HTTP server with wildcard matching for captive portal
    httpd_config_t config  = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.stack_size       = 6144;
    config.uri_match_fn     = httpd_uri_match_wildcard;

    if (httpd_start(&s_prov_server, &config) == ESP_OK) {
        // Exact-match handlers (checked before wildcard)
        httpd_uri_t u_root     = { "/",                    HTTP_GET,  root_handler,    NULL };
        httpd_uri_t u_scan     = { "/scan",                HTTP_GET,  scan_handler,    NULL };
        httpd_uri_t u_connect  = { "/connect",             HTTP_POST, connect_handler, NULL };
        // iOS CNA probe: must return 200 + portal HTML (body must NOT contain "Success")
        httpd_uri_t u_apple    = { "/hotspot-detect.html", HTTP_GET,  root_handler,    NULL };
        // Android probe: returning non-204 triggers captive portal notification
        httpd_uri_t u_gen204   = { "/generate_204",        HTTP_GET,  root_handler,    NULL };
        // Windows NCSI probe: redirect to logout.net opens the real browser
        httpd_uri_t u_ncsi     = { "/connecttest.txt",     HTTP_GET,  ncsi_handler,    NULL };
        // Catch-all wildcards (registered last, lowest priority)
        // Handle both GET and POST - POST /chat etc. from Android apps caused 405
        httpd_uri_t u_all      = { "/*", HTTP_GET,  captive_handler, NULL };
        httpd_uri_t u_all_post = { "/*", HTTP_POST, captive_handler, NULL };
        httpd_register_uri_handler(s_prov_server, &u_root);
        httpd_register_uri_handler(s_prov_server, &u_scan);
        httpd_register_uri_handler(s_prov_server, &u_connect);
        httpd_register_uri_handler(s_prov_server, &u_apple);
        httpd_register_uri_handler(s_prov_server, &u_gen204);
        httpd_register_uri_handler(s_prov_server, &u_ncsi);
        httpd_register_uri_handler(s_prov_server, &u_all);
        httpd_register_uri_handler(s_prov_server, &u_all_post);
        ESP_LOGI(TAG, "Provisioning HTTP server started");
    }

    // AP starts last - DNS and HTTP are already listening when first client connects
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Provisioning AP: %s  →  connect and open http://192.168.4.1/", ap_name);
}

// --- Public API ---

void wifi_manager_start(void)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(loop_err);

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &event_handler, NULL));

    // Check for saved credentials
    wifi_config_t sta_cfg = {0};
    esp_wifi_get_config(WIFI_IF_STA, &sta_cfg);
    bool provisioned = (strlen((char *)sta_cfg.sta.ssid) > 0);

    if (!provisioned) {
        s_provisioning = true;
        start_provisioning();
        // connect_handler calls esp_restart() on success;
        // this task waits indefinitely while the HTTP server handles provisioning
        vTaskDelay(portMAX_DELAY);
    }

    // Normal STA connection path
    ESP_LOGI(TAG, "Saved credentials: SSID=%s, connecting...", sta_cfg.sta.ssid);
    oled_set_line(2, "Connecting...");
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    // WIFI_EVENT_STA_START → event_handler → esp_wifi_connect()

    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi ready");
}

void wifi_manager_reset(void)
{
    ESP_LOGI(TAG, "Erasing WiFi credentials, rebooting into provisioning mode...");

    // Save current SSID for pre-selection in next provisioning session (Variant B)
    wifi_config_t cfg = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0]) {
        nvs_handle_t h;
        if (nvs_open(NVS_WIFI_PREV_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, NVS_WIFI_PREV_KEY, (char *)cfg.sta.ssid);
            nvs_commit(h);
            nvs_close(h);
            ESP_LOGI(TAG, "Saved previous SSID: %s", (char *)cfg.sta.ssid);
        }
    }

    esp_wifi_restore();
    vTaskDelay(pdMS_TO_TICKS(500)); // Allow HTTP/BLE response to be sent
    esp_restart();
}
