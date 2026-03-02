#include "led_controller.h"
#include "config.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "nvs.h"

#define LED_NVS_NS    "led_ctrl"
#define LED_NVS_KEY   "color"
#define MORSE_NVS_NS  "morse_cfg"
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
    LED_ANIM_HEARTBEAT,
    LED_ANIM_BREATHE,
    LED_ANIM_MORSE,
} led_anim_t;

static led_strip_handle_t s_led        = NULL;
static SemaphoreHandle_t  s_mutex      = NULL;
static TimerHandle_t      s_flash_tmr  = NULL;

static led_mode_t  s_mode        = LED_MODE_STATUS;
static led_anim_t  s_anim        = LED_ANIM_NONE;
static bool        s_connected   = false;
static char        s_cached_cmd[12] = "off"; // current command string for BLE read
static char        s_morse_text[BLE_MAX_VALUE_LEN + 1] = {0};
static morse_cfg_t s_morse_cfg;               // initialized in led_ctrl_init()

// --- NVS helpers ---

static void nvs_save_color(const char *hex6)
{
    nvs_handle_t h;
    if (nvs_open(LED_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, LED_NVS_KEY, hex6);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load_morse_cfg(morse_cfg_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(MORSE_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint16_t v;
    if (nvs_get_u16(h, "t1", &v) == ESP_OK) cfg->t1_ms = v;
    if (nvs_get_u16(h, "t2", &v) == ESP_OK) cfg->t2_ms = v;
    if (nvs_get_u16(h, "t3", &v) == ESP_OK) cfg->t3_ms = v;
    nvs_close(h);
}

static void nvs_save_morse_cfg(const morse_cfg_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(MORSE_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, "t1", cfg->t1_ms);
    nvs_set_u16(h, "t2", cfg->t2_ms);
    nvs_set_u16(h, "t3", cfg->t3_ms);
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

// --- Morse code support ---

// Russian А-Я transliteration (indices 0-31: А=0 … Я=31)
// Soft sign (Ь, index 29) and hard sign (Ъ, index 27) produce no Morse output.
static const char *s_ru_translit[32] = {
    "A","B","V","G","D","E","ZH","Z","I","Y",
    "K","L","M","N","O","P","R","S","T","U",
    "F","H","TS","CH","SH","SCH","","Y","","E",
    "YU","YA"
};

// Decode a single UTF-8 Cyrillic codepoint at in[*pos], advance *pos,
// and return the transliteration string (or NULL for unknown/punctuation).
static const char *decode_cyrillic(const char *in, size_t len, size_t *pos)
{
    uint8_t b0 = (uint8_t)in[*pos];
    if (*pos + 1 >= len) { (*pos)++; return NULL; }
    uint8_t b1 = (uint8_t)in[*pos + 1];
    *pos += 2;

    if (b0 == 0xD0) {
        if (b1 == 0x81) return s_ru_translit[5];          // Ё → Е
        if (b1 >= 0x90 && b1 <= 0xAF) return s_ru_translit[b1 - 0x90]; // А-Я
        if (b1 >= 0xB0 && b1 <= 0xBF) return s_ru_translit[b1 - 0xB0]; // а-п
    } else if (b0 == 0xD1) {
        if (b1 == 0x91) return s_ru_translit[5];          // ё → е
        if (b1 >= 0x80 && b1 <= 0x8F) return s_ru_translit[b1 - 0x80 + 16]; // р-я
    }
    return NULL;
}

// Transliterate UTF-8 text (Cyrillic → Latin, ASCII uppercase, skip other)
// into out[out_len]. Spaces are preserved.
static void morse_translit(const char *in, char *out, size_t out_len)
{
    size_t in_len  = strlen(in);
    size_t out_pos = 0;
    size_t i = 0;

    while (i < in_len && out_pos + 1 < out_len) {
        uint8_t c = (uint8_t)in[i];
        if (c == ' ') {
            out[out_pos++] = ' ';
            i++;
        } else if (c < 0x80) {
            // ASCII: pass through as uppercase
            if (c >= 'a' && c <= 'z') c -= 32;
            out[out_pos++] = (char)c;
            i++;
        } else if ((c == 0xD0 || c == 0xD1) && i + 1 < in_len) {
            const char *tr = decode_cyrillic(in, in_len, &i);
            if (tr) {
                size_t tlen = strlen(tr);
                if (out_pos + tlen < out_len) {
                    memcpy(out + out_pos, tr, tlen);
                    out_pos += tlen;
                }
            }
        } else {
            i++;  // skip other multibyte sequences
        }
    }
    out[out_pos] = '\0';
}

// A-Z then 0-9
static const char *s_morse_table[36] = {
    ".-",   "-...", "-.-.", "-..",  ".",    "..-.", "--.",  "....", "..",   ".---",
    "-.-",  ".-..", "--",   "-.",   "---",  ".--.", "--.-", ".-.",  "...",  "-",
    "..-",  "...-", ".--",  "-..-", "-.--", "--..",
    "-----",".----","..---","...--","....-",".....","-....","--...","---..", "----."
};

static const char *morse_encode(char c)
{
    if (c >= 'A' && c <= 'Z') return s_morse_table[c - 'A'];
    if (c >= 'a' && c <= 'z') return s_morse_table[c - 'a'];
    if (c >= '0' && c <= '9') return s_morse_table[26 + (c - '0')];
    return NULL;
}

static bool morse_is_active(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool active = (s_mode == LED_MODE_DEMO && s_anim == LED_ANIM_MORSE);
    xSemaphoreGive(s_mutex);
    return active;
}

static void morse_on(uint32_t ms)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_mode == LED_MODE_DEMO && s_anim == LED_ANIM_MORSE) {
        uint8_t r, g, b;
        hsv_to_rgb(28, 255, LED_DEMO_BRIGHTNESS, &r, &g, &b);  // warm amber
        set_raw(r, g, b);
    }
    xSemaphoreGive(s_mutex);
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void morse_off(uint32_t ms)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_mode == LED_MODE_DEMO && s_anim == LED_ANIM_MORSE) set_off();
    xSemaphoreGive(s_mutex);
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// Chunked wait for long pauses - checks for interruption every 200 ms
static void morse_wait(uint32_t ms)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_mode == LED_MODE_DEMO && s_anim == LED_ANIM_MORSE) set_off();
    xSemaphoreGive(s_mutex);
    while (ms > 0 && morse_is_active()) {
        uint32_t chunk = (ms > 200) ? 200 : ms;
        vTaskDelay(pdMS_TO_TICKS(chunk));
        ms -= chunk;
    }
}

static void play_morse(const char *text, const morse_cfg_t *cfg)
{
    // Derive actual on/off durations from decoder thresholds (with margin):
    //   dot  = 60% T1       → safely below T1 (dot/dash boundary)
    //   dash = 250% T1      → comfortably above T1
    //   sym  = 40% T2       → safely below T2 (sym/letter boundary)
    //   char = mid(T2, T3)  → falls in the letter-gap zone (T2 < char ≤ T3)
    //   word = char × 7/3   → standard Morse 7:3 ratio; adaptive decoders
    //                          cluster gaps by ratio, so ~2.3× char is needed
    uint32_t dot_ms      = (uint32_t)cfg->t1_ms * 6 / 10;
    uint32_t dash_ms     = (uint32_t)cfg->t1_ms * 5 / 2;
    uint32_t sym_gap_ms  = (uint32_t)cfg->t2_ms * 4 / 10;
    uint32_t char_gap_ms = ((uint32_t)cfg->t2_ms + cfg->t3_ms) / 2;
    uint32_t word_gap_ms = char_gap_ms * 7 / 3;

    // Transliterate Cyrillic → Latin (ASCII passes through unchanged)
    char xlat[128];
    morse_translit(text, xlat, sizeof(xlat));

    // Initial silence so the decoder establishes a clear baseline
    morse_off(char_gap_ms);

    for (int i = 0; xlat[i]; i++) {
        if (!morse_is_active()) return;
        char c = xlat[i];
        if (c == ' ') {
            // char_gap was already added after the previous character;
            // add only the extra silence to reach word_gap total.
            if (word_gap_ms > char_gap_ms)
                morse_off(word_gap_ms - char_gap_ms);
        } else {
            const char *code = morse_encode(c);
            if (!code) continue;
            for (int j = 0; code[j]; j++) {
                if (code[j] == '.') {
                    morse_on(dot_ms);
                } else {
                    morse_on(dash_ms);
                }
                if (code[j + 1]) {
                    morse_off(sym_gap_ms);   // gap between elements within char
                }
            }
            morse_off(char_gap_ms);          // gap after each character
        }
    }
    if (morse_is_active()) {
        morse_wait(3000);  // pause between repeats, interruptible every 200 ms
    }
}

// --- Animation task (33 ms / frame ≈ 30 fps) ---

static void anim_task(void *arg)
{
    uint16_t hue       = 0;
    uint32_t fire_seed = 0xDEADBEEF;
    uint8_t  fire_val  = LED_DEMO_BRIGHTNESS;

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
            // LCG pseudo-random; organic flicker: decay + random spikes
            fire_seed = fire_seed * 1664525UL + 1013904223UL;
            uint8_t rnd   = (uint8_t)(fire_seed >> 16);
            uint8_t decay = 6 + (rnd & 7);  // decay 6-13 per frame
            fire_val = (fire_val > decay) ? fire_val - decay : 0;
            if (rnd < 60) {  // ~23% chance: spike brightness
                fire_seed = fire_seed * 1664525UL + 1013904223UL;
                uint8_t rnd2 = (uint8_t)(fire_seed >> 24);
                fire_val = LED_DEMO_BRIGHTNESS / 3
                           + (rnd2 % (LED_DEMO_BRIGHTNESS * 2 / 3 + 1));
            }
            if (fire_val < LED_DEMO_BRIGHTNESS / 4) fire_val = LED_DEMO_BRIGHTNESS / 4;
            uint16_t fire_hue = rnd % 13;  // hue 0-12: deep red to barely orange
            hsv_to_rgb(fire_hue, 255, fire_val, &r, &g, &b);
            break;
        }

        case LED_ANIM_HEARTBEAT: {
            // 34-frame cycle (~1.1 s): strong beat, short gap, softer beat, long rest
            uint8_t phase = (uint8_t)(hue % 34);
            hue++;
            if (phase < 3) {
                hsv_to_rgb(5, 255, LED_DEMO_BRIGHTNESS, &r, &g, &b);          // beat 1
            } else if (phase < 5) {
                r = g = b = 0;                                                  // gap
            } else if (phase < 9) {
                hsv_to_rgb(5, 255, LED_DEMO_BRIGHTNESS * 2 / 3, &r, &g, &b); // beat 2
            }
            // phase 9-33: r=g=b=0 (rest, already zero-initialized)
            break;
        }

        case LED_ANIM_BREATHE: {
            // 120-frame cycle (~4 s): triangle-wave brightness, cool blue
            uint8_t phase = (uint8_t)(hue % 120);
            uint8_t x     = (phase < 60) ? phase : (119 - phase);  // 0..59
            uint8_t val   = LED_DEMO_BRIGHTNESS / 6
                            + (uint8_t)((uint16_t)x * (LED_DEMO_BRIGHTNESS * 5 / 6) / 59);
            hsv_to_rgb(200, 200, val, &r, &g, &b);
            hue = (hue + 1) % 120;
            break;
        }

        case LED_ANIM_MORSE: {
            char text[BLE_MAX_VALUE_LEN + 1];
            strncpy(text, s_morse_text, BLE_MAX_VALUE_LEN);
            text[BLE_MAX_VALUE_LEN] = '\0';
            morse_cfg_t cfg = s_morse_cfg;    // snapshot config under mutex
            xSemaphoreGive(s_mutex);          // release before long blocking playback
            if (text[0] != '\0') {
                play_morse(text, &cfg);
            } else {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            continue;  // mutex already released; skip set_raw/give/delay below
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

    // Initialize Morse thresholds from compiled-in defaults, then overlay NVS
    s_morse_cfg.t1_ms = MORSE_DEFAULT_T1_MS;
    s_morse_cfg.t2_ms = MORSE_DEFAULT_T2_MS;
    s_morse_cfg.t3_ms = MORSE_DEFAULT_T3_MS;
    nvs_load_morse_cfg(&s_morse_cfg);

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
    if (strcmp(cmd, "heartbeat") == 0) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_mode = LED_MODE_DEMO;
        s_anim = LED_ANIM_HEARTBEAT;
        strncpy(s_cached_cmd, "heartbeat", sizeof(s_cached_cmd) - 1);
        xSemaphoreGive(s_mutex);
        return true;
    }
    if (strcmp(cmd, "breathe") == 0) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_mode = LED_MODE_DEMO;
        s_anim = LED_ANIM_BREATHE;
        strncpy(s_cached_cmd, "breathe", sizeof(s_cached_cmd) - 1);
        xSemaphoreGive(s_mutex);
        return true;
    }
    if (strcmp(cmd, "morse") == 0) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_mode = LED_MODE_DEMO;
        s_anim = LED_ANIM_MORSE;
        strncpy(s_cached_cmd, "morse", sizeof(s_cached_cmd) - 1);
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

void led_ctrl_set_morse_text(const char *text)
{
    if (!text) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_morse_text, text, BLE_MAX_VALUE_LEN);
    s_morse_text[BLE_MAX_VALUE_LEN] = '\0';
    xSemaphoreGive(s_mutex);
}

void led_ctrl_get_morse_timing(morse_cfg_t *cfg)
{
    if (!cfg) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *cfg = s_morse_cfg;
    xSemaphoreGive(s_mutex);
}

void led_ctrl_set_morse_timing(const morse_cfg_t *cfg)
{
    if (!cfg) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_morse_cfg = *cfg;
    xSemaphoreGive(s_mutex);
    nvs_save_morse_cfg(cfg);  // persist outside mutex (flash write can be slow)
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
