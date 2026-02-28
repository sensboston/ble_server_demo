#include "led_controller.h"
#include "config.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "nvs.h"

#define LED_NVS_NS  "led_ctrl"
#define LED_NVS_KEY "color"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"

#define TAG "LED_CTRL"

typedef enum {
    LED_MODE_STATUS = 0,  // BLE status indication (default)
    LED_MODE_DEMO,        // user-controlled color or animation
} led_mode_t;

typedef enum {
    LED_ANIM_NONE = 0,
    LED_ANIM_FADE,
    LED_ANIM_FIRE,
    LED_ANIM_RAINBOW,
} led_anim_t;

static led_strip_handle_t s_led        = NULL;
static SemaphoreHandle_t  s_mutex      = NULL;
static TimerHandle_t      s_flash_tmr  = NULL;

static led_mode_t s_mode        = LED_MODE_STATUS;
static led_anim_t s_anim        = LED_ANIM_NONE;
static bool       s_connected   = false;
static char       s_cached_cmd[9] = "off"; // current command string for BLE read

// --- NVS helpers ---

static void nvs_save_color(const char *hex6)
{
    nvs_handle_t h;
    if (nvs_open(LED_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, LED_NVS_KEY, hex6);
    nvs_commit(h);
    nvs_close(h);
}

// --- Low-level LED write (always call with s_mutex held) ---

static void set_raw(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(s_led, 0, r, g, b);
    led_strip_refresh(s_led);
}

static void set_off(void)
{
    led_strip_clear(s_led);
    led_strip_refresh(s_led);
}

// --- HSV → RGB (h: 0-359, s: 0-255, v: 0-255) ---

static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v,
                       uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (s == 0) { *r = *g = *b = v; return; }
    uint16_t region    = h / 60;
    uint16_t remainder = (h - region * 60) * 255 / 60;
    uint8_t  p = (uint16_t)v * (255 - s) / 255;
    uint8_t  q = (uint16_t)v * (255 - ((uint16_t)s * remainder / 255)) / 255;
    uint8_t  t = (uint16_t)v * (255 - ((uint16_t)s * (255 - remainder) / 255)) / 255;
    switch (region) {
    case 0: *r = v; *g = t; *b = p; break;
    case 1: *r = q; *g = v; *b = p; break;
    case 2: *r = p; *g = v; *b = t; break;
    case 3: *r = p; *g = q; *b = v; break;
    case 4: *r = t; *g = p; *b = v; break;
    default:*r = v; *g = p; *b = q; break;
    }
}

// --- Flash timer callback: restore status LED after a BLE event flash ---

static void flash_timer_cb(TimerHandle_t xTimer)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_mode == LED_MODE_STATUS) {
        if (s_connected)
            set_raw(0, LED_BRIGHTNESS, 0);
        else
            set_off();
    }
    xSemaphoreGive(s_mutex);
}

// --- Animation task (33 ms / frame ≈ 30 fps) ---

static void anim_task(void *arg)
{
    uint16_t hue       = 0;
    uint32_t fire_seed = 0xDEADBEEF;

    for (;;) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);

        if (s_mode != LED_MODE_DEMO || s_anim == LED_ANIM_NONE) {
            xSemaphoreGive(s_mutex);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uint8_t r = 0, g = 0, b = 0;

        switch (s_anim) {
        case LED_ANIM_FADE:
            // Full hue cycle in ~12 s (360 steps × 33 ms)
            hsv_to_rgb(hue, 255, LED_DEMO_BRIGHTNESS, &r, &g, &b);
            hue = (hue + 1) % 360;
            break;

        case LED_ANIM_RAINBOW:
            // Full hue cycle in ~3 s (90 steps × 33 ms)
            hsv_to_rgb(hue, 255, LED_DEMO_BRIGHTNESS, &r, &g, &b);
            hue = (hue + 4) % 360;
            break;

        case LED_ANIM_FIRE: {
            // LCG pseudo-random, hue 0-25 (red→orange), varying brightness
            fire_seed = fire_seed * 1664525UL + 1013904223UL;
            uint8_t  rnd      = (uint8_t)(fire_seed >> 16);
            uint16_t fire_hue = rnd % 26;
            uint8_t  fire_val = LED_DEMO_BRIGHTNESS / 2
                                + (rnd % (LED_DEMO_BRIGHTNESS / 2 + 1));
            hsv_to_rgb(fire_hue, 230, fire_val, &r, &g, &b);
            break;
        }

        default:
            break;
        }

        set_raw(r, g, b);
        xSemaphoreGive(s_mutex);
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}

// --- Public API ---

void led_ctrl_init(led_strip_handle_t led)
{
    s_led   = led;
    s_mutex = xSemaphoreCreateMutex();

    s_flash_tmr = xTimerCreate("led_flash",
                               pdMS_TO_TICKS(LED_FLASH_DURATION_MS),
                               pdFALSE, NULL, flash_timer_cb);

    // Restore saved color from NVS (before task starts; no concurrency yet)
    char saved[9] = {0};
    size_t len = sizeof(saved);
    nvs_handle_t h;
    if (nvs_open(LED_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, LED_NVS_KEY, saved, &len) == ESP_OK)
            led_ctrl_apply_command(saved); // applies if valid hex, ignored otherwise
        nvs_close(h);
    }

    xTaskCreate(anim_task, "led_anim", LED_ANIM_TASK_STACK, NULL, 3, NULL);

    ESP_LOGI(TAG, "LED controller initialized");
}

void led_ctrl_get_command(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(buf, s_cached_cmd, len - 1);
    buf[len - 1] = '\0';
    xSemaphoreGive(s_mutex);
}

bool led_ctrl_apply_command(const char *cmd)
{
    if (!cmd) return false;

    // Named animation / off commands
    if (strcmp(cmd, "off") == 0) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_mode = LED_MODE_STATUS;
        s_anim = LED_ANIM_NONE;
        strncpy(s_cached_cmd, "off", sizeof(s_cached_cmd) - 1);
        if (s_connected) set_raw(0, LED_BRIGHTNESS, 0); else set_off();
        xSemaphoreGive(s_mutex);
        return true;
    }
    if (strcmp(cmd, "fade") == 0) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_mode = LED_MODE_DEMO;
        s_anim = LED_ANIM_FADE;
        strncpy(s_cached_cmd, "fade", sizeof(s_cached_cmd) - 1);
        xSemaphoreGive(s_mutex);
        return true;
    }
    if (strcmp(cmd, "fire") == 0) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_mode = LED_MODE_DEMO;
        s_anim = LED_ANIM_FIRE;
        strncpy(s_cached_cmd, "fire", sizeof(s_cached_cmd) - 1);
        xSemaphoreGive(s_mutex);
        return true;
    }
    if (strcmp(cmd, "rainbow") == 0) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_mode = LED_MODE_DEMO;
        s_anim = LED_ANIM_RAINBOW;
        strncpy(s_cached_cmd, "rainbow", sizeof(s_cached_cmd) - 1);
        xSemaphoreGive(s_mutex);
        return true;
    }

    // "RRGGBB" static color
    if (strlen(cmd) == 6) {
        for (int i = 0; i < 6; i++) {
            char c = cmd[i];
            if (!((c >= '0' && c <= '9') ||
                  (c >= 'A' && c <= 'F') ||
                  (c >= 'a' && c <= 'f')))
                return false;
        }
        char tmp[3] = {0};
        tmp[0] = cmd[0]; tmp[1] = cmd[1];
        uint8_t r = (uint8_t)strtol(tmp, NULL, 16);
        tmp[0] = cmd[2]; tmp[1] = cmd[3];
        uint8_t g = (uint8_t)strtol(tmp, NULL, 16);
        tmp[0] = cmd[4]; tmp[1] = cmd[5];
        uint8_t b = (uint8_t)strtol(tmp, NULL, 16);

        // Scale 0-255 → 0-LED_DEMO_BRIGHTNESS so hue is preserved but safe brightness
        r = (uint16_t)r * LED_DEMO_BRIGHTNESS / 255;
        g = (uint16_t)g * LED_DEMO_BRIGHTNESS / 255;
        b = (uint16_t)b * LED_DEMO_BRIGHTNESS / 255;

        // Copy hex string before taking mutex (for NVS save after release)
        char hex_save[7];
        memcpy(hex_save, cmd, 6);
        hex_save[6] = '\0';

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_mode = LED_MODE_DEMO;
        s_anim = LED_ANIM_NONE;
        memcpy(s_cached_cmd, hex_save, 7);
        set_raw(r, g, b);
        xSemaphoreGive(s_mutex);

        nvs_save_color(hex_save); // persist outside mutex (flash write can be slow)
        return true;
    }

    return false;
}

void led_ctrl_ble_connected(bool connected)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_connected = connected;
    if (s_mode == LED_MODE_STATUS) {
        if (connected) set_raw(0, LED_BRIGHTNESS, 0); else set_off();
    }
    xSemaphoreGive(s_mutex);
}

void led_ctrl_ble_flash(bool is_read)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_mode == LED_MODE_STATUS) {
        // blue = read, red = write
        if (is_read) set_raw(0, 0, LED_BRIGHTNESS);
        else         set_raw(LED_BRIGHTNESS, 0, 0);
        xTimerReset(s_flash_tmr, 0);
    }
    xSemaphoreGive(s_mutex);
}
