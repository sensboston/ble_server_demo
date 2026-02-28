#include <stdio.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ble_server.h"
#include "wifi_manager.h"
#include "web_server.h"

#define TAG "MAIN"

// WS2812 RGB LED GPIO
#define LED_GPIO    8

// Task stack sizes
#define BLE_TASK_STACK  4096
#define WIFI_TASK_STACK 6144

static led_strip_handle_t led_strip;

// WiFi manager task - runs provisioning or connects, then starts web server
static void wifi_task(void *arg)
{
    wifi_manager_start();   // Blocks until WiFi connected
    web_server_start();     // Start HTTP server after WiFi is ready
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

    // Initialize WS2812 RGB LED on GPIO8
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

    // Initialize BLE event logging mutex before any tasks start
    web_log_init();

    // Start BLE and WiFi as independent FreeRTOS tasks
    xTaskCreate(ble_task,  "ble_task",  BLE_TASK_STACK,  NULL, 5, NULL);
    xTaskCreate(wifi_task, "wifi_task", WIFI_TASK_STACK, NULL, 5, NULL);

    ESP_LOGI(TAG, "Tasks started");
}