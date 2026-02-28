#include <stdio.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"
#include "ble_server.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "ntp_sync.h"
#include "oled_display.h"


#define TAG "MAIN"

static led_strip_handle_t led_strip;

// WiFi manager task - connects to WiFi, syncs time, then starts web server
static void wifi_task(void *arg)
{
    wifi_manager_start();   // Blocks until WiFi connected
    ntp_sync_start();       // Start NTP sync (runs in background)
    web_server_start();     // Start HTTP server
    vTaskDelete(NULL);
}

// BLE server task - runs GATT server independently of WiFi
static void ble_task(void *arg)
{
    ble_server_start(led_strip);
    vTaskDelete(NULL);
}

void app_main(void)
{
    // Initialize NVS - required for both BT stack and WiFi credentials storage
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    // Initialize WS2812 RGB LED
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);
    led_strip_refresh(led_strip);
    ESP_LOGI(TAG, "LED initialized");

    // Initialize OLED display
    if (oled_init() == ESP_OK) {
        oled_set_line(0, BLE_DEVICE_NAME);
        oled_set_line(1, "BLE: Init...");
        oled_set_line(2, "WiFi: ...");
    }

    // Initialize BLE event logging mutex, load persisted config
    web_log_init();
    web_set_ble_ctrl_cb(ble_set_enabled);
    web_set_wifi_reset_cb(wifi_manager_reset);
    ble_set_wifi_reset_cb(wifi_manager_reset);

    // Start BLE and WiFi as independent FreeRTOS tasks
    xTaskCreate(ble_task,  "ble_task",  BLE_TASK_STACK,  NULL, 5, NULL);
    xTaskCreate(wifi_task, "wifi_task", WIFI_TASK_STACK, NULL, 5, NULL);

    ESP_LOGI(TAG, "Tasks started");
}
