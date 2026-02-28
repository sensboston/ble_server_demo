#include "wifi_manager.h"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"

#define TAG "WIFI_MANAGER"

// Event bit for signaling WiFi connection
#define WIFI_CONNECTED_BIT  BIT0

static EventGroupHandle_t wifi_event_group;

// --- Event handler for WiFi, IP and provisioning events ---
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
        case WIFI_PROV_START:
            ESP_LOGI(TAG, "Provisioning started, connect to AP and open 192.168.4.1");
            break;
        case WIFI_PROV_CRED_RECV: {
            wifi_sta_config_t *cfg = (wifi_sta_config_t *)event_data;
            ESP_LOGI(TAG, "Received credentials - SSID: %s", (const char *)cfg->ssid);
            break;
        }
        case WIFI_PROV_CRED_FAIL: {
            wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
            ESP_LOGE(TAG, "Provisioning failed: %s",
                     (*reason == WIFI_PROV_STA_AUTH_ERROR) ?
                     "Authentication failed" : "AP not found");
            // Reset provisioning so user can try again
            wifi_prov_mgr_reset_provisioning();
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "Provisioning successful");
            break;
        case WIFI_PROV_END:
            // Release provisioning manager resources after completion
            wifi_prov_mgr_deinit();
            break;
        default:
            break;
        }
    } else if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGI(TAG, "Disconnected, reconnecting...");
            esp_wifi_connect();
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "Device connected to provisioning AP");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "Device disconnected from provisioning AP");
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Build unique AP name using last 3 bytes of MAC address
static void get_service_name(char *name, size_t max)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(name, max, "ESP32_%02X%02X%02X", mac[3], mac[4], mac[5]);
}

// Initialize WiFi - provisioning if needed, STA mode if already provisioned
void wifi_manager_start(void)
{
    wifi_event_group = xEventGroupCreate();

    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default event loop (ESP_ERR_INVALID_STATE means it already exists)
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop_err);
    }

    // Create default netif interfaces for STA and AP modes
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,      ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,        IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    // Configure provisioning manager with SoftAP scheme
    wifi_prov_mgr_config_t prov_config = {
        .scheme               = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };
    ESP_ERROR_CHECK(wifi_prov_mgr_init(prov_config));

    // Check if device is already provisioned
    bool provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    if (!provisioned) {
        // Start provisioning via SoftAP
        char service_name[16];
        get_service_name(service_name, sizeof(service_name));
        ESP_LOGI(TAG, "Starting provisioning AP: %s", service_name);
        ESP_LOGI(TAG, "Connect to this AP and open http://192.168.4.1 in browser");

        // Security 0 = no encryption, simplest setup for local network provisioning
        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(
            WIFI_PROV_SECURITY_0, NULL, service_name, NULL));
    } else {
        // Already provisioned - just start WiFi in STA mode
        ESP_LOGI(TAG, "Already provisioned, connecting to WiFi...");
        wifi_prov_mgr_deinit();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    // Wait for WiFi connection (non-blocking for BLE - runs in separate task)
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG, "WiFi ready");
}