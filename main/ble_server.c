#include <string.h>
#include "ble_server.h"
#include "web_server.h"
#include "config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#define TAG "BLE_SERVER"

// GATTS application profile ID
#define PROFILE_APP_ID      0

// NVS namespace and key for persistent storage
#define NVS_NAMESPACE       "ble_storage"
#define NVS_KEY             "ble_value"

// In-RAM cache for the characteristic value to avoid NVS reads on every BLE READ
static char   cached_value[BLE_MAX_VALUE_LEN + 1];
static size_t cached_len;

static uint16_t service_handle;
static uint16_t char_handle;
static uint16_t reset_char_handle;

static ble_wifi_reset_cb_t s_wifi_reset_cb = NULL;

// Current connected client address (for logging)
static uint8_t connected_bd_addr[6];

// LED strip handle - received from main
static led_strip_handle_t led_strip;

// FreeRTOS timer for returning LED to connected state after flash
static TimerHandle_t led_timer;

// Track connection state for timer callback and enable/disable control
static bool              is_connected     = false;
static bool              s_ble_enabled    = true;
static uint16_t          current_conn_id  = 0xFFFF;
static esp_gatt_if_t     current_gatts_if = 0xFF;

// --- LED helper functions ---

static void led_off(void)
{
    led_strip_clear(led_strip);
    led_strip_refresh(led_strip);
}

static void led_connected(void)
{
    led_strip_set_pixel(led_strip, 0, 0, LED_BRIGHTNESS, 0);
    led_strip_refresh(led_strip);
}

static void led_read(void)
{
    led_strip_set_pixel(led_strip, 0, 0, 0, LED_BRIGHTNESS);
    led_strip_refresh(led_strip);
}

static void led_write(void)
{
    led_strip_set_pixel(led_strip, 0, LED_BRIGHTNESS, 0, 0);
    led_strip_refresh(led_strip);
}

// Timer callback - restore connected color after flash
static void led_timer_callback(TimerHandle_t xTimer)
{
    if (is_connected)
        led_connected();
    else
        led_off();
}

// Flash LED for operation, then restore after delay
static void led_flash_operation(bool is_read)
{
    if (is_read)
        led_read();
    else
        led_write();

    // Reset and start one-shot timer to restore color
    xTimerReset(led_timer, 0);
}

// --- Advertising parameters - global to reuse on reconnect ---
static esp_ble_adv_params_t adv_params = {
    .adv_int_min        = 0x20,
    .adv_int_max        = 0x40,
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// --- NVS helper functions ---

static esp_err_t nvs_read_value(char *buf, size_t *len)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) return ret;

    ret = nvs_get_str(handle, NVS_KEY, buf, len);
    nvs_close(handle);
    return ret;
}

static esp_err_t nvs_write_value(const char *buf)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_str(handle, NVS_KEY, buf);
    if (ret == ESP_OK)
        ret = nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

// GAP event handler - manages advertising and security events
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS)
            ESP_LOGI(TAG, "Advertising started");
        else
            ESP_LOGE(TAG, "Advertising start failed");
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        ESP_LOGI(TAG, "Advertising stopped");
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        // Reject all pairing requests - this device does not support pairing
        ESP_LOGI(TAG, "Security request received, rejecting pairing");
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, false);
        break;

    default:
        break;
    }
}

// GATTS event handler - manages GATT server events
static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                 esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT: {
        ESP_LOGI(TAG, "GATTS registered, app_id: %d", param->reg.app_id);
        esp_ble_gap_set_device_name(BLE_DEVICE_NAME);

        // Create service with 4 handles (service + characteristic + descriptor)
        esp_gatt_srvc_id_t service_id = {
            .is_primary = true,
            .id = {
                .inst_id = 0,
                .uuid = {
                    .len = ESP_UUID_LEN_16,
                    .uuid = { .uuid16 = BLE_SERVICE_UUID }
                }
            }
        };
        esp_ble_gatts_create_service(gatts_if, &service_id, 8);
        break;
    }
    case ESP_GATTS_CREATE_EVT: {
        ESP_LOGI(TAG, "Service created, handle: %d", param->create.service_handle);
        service_handle = param->create.service_handle;

        esp_ble_gatts_start_service(service_handle);

        // Add characteristic with read/write permissions
        esp_bt_uuid_t char_uuid = {
            .len = ESP_UUID_LEN_16,
            .uuid = { .uuid16 = BLE_CHAR_UUID }
        };

        // Load value from NVS into cache, use default if not found
        cached_len = sizeof(cached_value);
        if (nvs_read_value(cached_value, &cached_len) != ESP_OK) {
            ESP_LOGI(TAG, "No NVS value found, using default: %s", BLE_DEFAULT_VALUE);
            strncpy(cached_value, BLE_DEFAULT_VALUE, sizeof(cached_value));
            cached_len = strlen(BLE_DEFAULT_VALUE);
        } else {
            ESP_LOGI(TAG, "Loaded value from NVS: %s", cached_value);
        }

        esp_attr_value_t char_val = {
            .attr_max_len = BLE_MAX_VALUE_LEN,
            .attr_len     = cached_len,
            .attr_value   = (uint8_t *)cached_value
        };
        esp_ble_gatts_add_char(service_handle, &char_uuid,
                               ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                               ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE,
                               &char_val, NULL);
        break;
    }
    case ESP_GATTS_ADD_CHAR_EVT: {
        uint16_t uuid16 = param->add_char.char_uuid.uuid.uuid16;
        ESP_LOGI(TAG, "Characteristic added, uuid: 0x%04X, handle: %d",
                 uuid16, param->add_char.attr_handle);
        if (uuid16 == BLE_CHAR_UUID) {
            char_handle = param->add_char.attr_handle;
            // Chain: add the WiFi-reset write-only characteristic
            esp_bt_uuid_t reset_uuid = {
                .len = ESP_UUID_LEN_16,
                .uuid = { .uuid16 = BLE_RESET_CHAR_UUID }
            };
            esp_ble_gatts_add_char(service_handle, &reset_uuid,
                                   ESP_GATT_PERM_WRITE,
                                   ESP_GATT_CHAR_PROP_BIT_WRITE,
                                   NULL, NULL);
        } else if (uuid16 == BLE_RESET_CHAR_UUID) {
            reset_char_handle = param->add_char.attr_handle;
        }
        break;
    }

    case ESP_GATTS_START_EVT: {
        ESP_LOGI(TAG, "Service started");

        // Configure advertising data
        esp_ble_adv_data_t adv_data = {
            .set_scan_rsp        = false,
            .include_name        = true,
            .include_txpower     = true,
            .min_interval        = 0x0006,
            .max_interval        = 0x0010,
            .appearance          = 0x00,
            .manufacturer_len    = 0,
            .p_manufacturer_data = NULL,
            .service_data_len    = 0,
            .p_service_data      = NULL,
            .service_uuid_len    = 0,
            .p_service_uuid      = NULL,
            .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
        };
        esp_ble_gap_config_adv_data(&adv_data);
        esp_ble_gap_start_advertising(&adv_params);
        break;
    }
    case ESP_GATTS_CONNECT_EVT:
        ESP_LOGI(TAG, "Client connected, conn_id: %d", param->connect.conn_id);
        is_connected    = true;
        current_conn_id = param->connect.conn_id;
        current_gatts_if = gatts_if;
        memcpy(connected_bd_addr, param->connect.remote_bda, 6);
        led_connected();
        web_log_connect(connected_bd_addr);
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        ESP_LOGI(TAG, "Client disconnected");
        is_connected    = false;
        current_conn_id = 0xFFFF;
        web_log_disconnect(connected_bd_addr);
        led_off();
        if (s_ble_enabled)
            esp_ble_gap_start_advertising(&adv_params);
        break;

    case ESP_GATTS_READ_EVT: {
        ESP_LOGI(TAG, "Read request, conn_id: %d, handle: %d",
                 param->read.conn_id, param->read.handle);

        // Flash blue for read operation
        led_flash_operation(true);

        web_log_read(connected_bd_addr, BLE_CHAR_UUID, cached_value);

        // Send cached value to client (no NVS access needed)
        esp_gatt_rsp_t rsp = {0};
        rsp.attr_value.handle = param->read.handle;
        rsp.attr_value.len    = cached_len;
        memcpy(rsp.attr_value.value, cached_value, cached_len);
        esp_ble_gatts_send_response(gatts_if, param->read.conn_id,
                                    param->read.trans_id, ESP_GATT_OK, &rsp);
        ESP_LOGI(TAG, "Read response sent: %s", cached_value);
        break;
    }
    case ESP_GATTS_WRITE_EVT: {
        ESP_LOGI(TAG, "Write request, conn_id: %d, handle: %d, len: %d",
                 param->write.conn_id, param->write.handle, param->write.len);

        if (param->write.handle == reset_char_handle) {
            // WiFi reset characteristic: value "1" triggers reset
            if (param->write.len >= 1 && param->write.value[0] == '1') {
                ESP_LOGI(TAG, "WiFi reset requested via BLE");
                if (param->write.need_rsp)
                    esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                                param->write.trans_id, ESP_GATT_OK, NULL);
                if (s_wifi_reset_cb) s_wifi_reset_cb();
            }
            break;
        }

        // Default characteristic: flash red, update cache, persist to NVS
        led_flash_operation(false);

        cached_len = param->write.len < BLE_MAX_VALUE_LEN ? param->write.len : BLE_MAX_VALUE_LEN;
        memcpy(cached_value, param->write.value, cached_len);
        cached_value[cached_len] = '\0';

        esp_err_t ret = nvs_write_value(cached_value);
        if (ret == ESP_OK)
            ESP_LOGI(TAG, "Value saved to NVS: %s", cached_value);
        else
            ESP_LOGE(TAG, "NVS write failed: %s", esp_err_to_name(ret));

        web_log_write(connected_bd_addr, BLE_CHAR_UUID, cached_value);

        if (param->write.need_rsp)
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                        param->write.trans_id, ESP_GATT_OK, NULL);
        break;
    }
    default:
        break;
    }
}

void ble_set_enabled(bool enabled)
{
    s_ble_enabled = enabled;
    if (!enabled) {
        esp_ble_gap_stop_advertising();
        if (current_conn_id != 0xFFFF)
            esp_ble_gatts_close(current_gatts_if, current_conn_id);
    } else {
        if (!is_connected)
            esp_ble_gap_start_advertising(&adv_params);
    }
}

bool ble_is_enabled(void)
{
    return s_ble_enabled;
}

void ble_set_wifi_reset_cb(ble_wifi_reset_cb_t cb)
{
    s_wifi_reset_cb = cb;
}

// Initialize and start BLE GATT server
void ble_server_start(led_strip_handle_t led)
{
    led_strip = led;

    // Create one-shot timer for restoring LED color after flash
    led_timer = xTimerCreate("led_timer",
                              pdMS_TO_TICKS(LED_FLASH_DURATION_MS),
                              pdFALSE,
                              NULL,
                              led_timer_callback);

    // Release Classic BT memory - ESP32-C3 supports BLE only
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    // Initialize BT controller with default config
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    // Initialize and enable Bluedroid stack
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_LOGI(TAG, "Bluetooth initialized");

    // Configure security - no bonding, no MITM, reject all pairing requests
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_NO_BOND;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));

    uint8_t iocap = ESP_IO_CAP_NONE;
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));

    // Register GAP and GATTS callbacks
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));

    // Register GATTS application profile
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(PROFILE_APP_ID));
}
