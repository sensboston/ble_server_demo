#include "oled_display.h"
#include "config.h"
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TAG "OLED"

// ---------------------------------------------------------------------------
// Standard 5×7 ASCII font, characters 0x20 (space) through 0x7E (~).
// Each entry is 5 column bytes; bit 0 = top pixel, bit 6 = bottom pixel.
// ---------------------------------------------------------------------------
static const uint8_t s_font[][5] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 }, // 0x20 ' '
    { 0x00, 0x00, 0x5F, 0x00, 0x00 }, // 0x21 '!'
    { 0x00, 0x07, 0x00, 0x07, 0x00 }, // 0x22 '"'
    { 0x14, 0x7F, 0x14, 0x7F, 0x14 }, // 0x23 '#'
    { 0x24, 0x2A, 0x7F, 0x2A, 0x12 }, // 0x24 '$'
    { 0x23, 0x13, 0x08, 0x64, 0x62 }, // 0x25 '%'
    { 0x36, 0x49, 0x55, 0x22, 0x50 }, // 0x26 '&'
    { 0x00, 0x05, 0x03, 0x00, 0x00 }, // 0x27 '\''
    { 0x00, 0x1C, 0x22, 0x41, 0x00 }, // 0x28 '('
    { 0x00, 0x41, 0x22, 0x1C, 0x00 }, // 0x29 ')'
    { 0x08, 0x2A, 0x1C, 0x2A, 0x08 }, // 0x2A '*'
    { 0x08, 0x08, 0x3E, 0x08, 0x08 }, // 0x2B '+'
    { 0x00, 0x50, 0x30, 0x00, 0x00 }, // 0x2C ','
    { 0x08, 0x08, 0x08, 0x08, 0x08 }, // 0x2D '-'
    { 0x00, 0x60, 0x60, 0x00, 0x00 }, // 0x2E '.'
    { 0x20, 0x10, 0x08, 0x04, 0x02 }, // 0x2F '/'
    { 0x3E, 0x51, 0x49, 0x45, 0x3E }, // 0x30 '0'
    { 0x00, 0x42, 0x7F, 0x40, 0x00 }, // 0x31 '1'
    { 0x42, 0x61, 0x51, 0x49, 0x46 }, // 0x32 '2'
    { 0x21, 0x41, 0x45, 0x4B, 0x31 }, // 0x33 '3'
    { 0x18, 0x14, 0x12, 0x7F, 0x10 }, // 0x34 '4'
    { 0x27, 0x45, 0x45, 0x45, 0x39 }, // 0x35 '5'
    { 0x3C, 0x4A, 0x49, 0x49, 0x30 }, // 0x36 '6'
    { 0x01, 0x71, 0x09, 0x05, 0x03 }, // 0x37 '7'
    { 0x36, 0x49, 0x49, 0x49, 0x36 }, // 0x38 '8'
    { 0x06, 0x49, 0x49, 0x29, 0x1E }, // 0x39 '9'
    { 0x00, 0x36, 0x36, 0x00, 0x00 }, // 0x3A ':'
    { 0x00, 0x56, 0x36, 0x00, 0x00 }, // 0x3B ';'
    { 0x00, 0x08, 0x14, 0x22, 0x41 }, // 0x3C '<'
    { 0x14, 0x14, 0x14, 0x14, 0x14 }, // 0x3D '='
    { 0x41, 0x22, 0x14, 0x08, 0x00 }, // 0x3E '>'
    { 0x02, 0x01, 0x51, 0x09, 0x06 }, // 0x3F '?'
    { 0x32, 0x49, 0x79, 0x41, 0x3E }, // 0x40 '@'
    { 0x7E, 0x11, 0x11, 0x11, 0x7E }, // 0x41 'A'
    { 0x7F, 0x49, 0x49, 0x49, 0x36 }, // 0x42 'B'
    { 0x3E, 0x41, 0x41, 0x41, 0x22 }, // 0x43 'C'
    { 0x7F, 0x41, 0x41, 0x22, 0x1C }, // 0x44 'D'
    { 0x7F, 0x49, 0x49, 0x49, 0x41 }, // 0x45 'E'
    { 0x7F, 0x09, 0x09, 0x01, 0x01 }, // 0x46 'F'
    { 0x3E, 0x41, 0x41, 0x49, 0x7A }, // 0x47 'G'
    { 0x7F, 0x08, 0x08, 0x08, 0x7F }, // 0x48 'H'
    { 0x00, 0x41, 0x7F, 0x41, 0x00 }, // 0x49 'I'
    { 0x20, 0x40, 0x41, 0x3F, 0x01 }, // 0x4A 'J'
    { 0x7F, 0x08, 0x14, 0x22, 0x41 }, // 0x4B 'K'
    { 0x7F, 0x40, 0x40, 0x40, 0x40 }, // 0x4C 'L'
    { 0x7F, 0x02, 0x04, 0x02, 0x7F }, // 0x4D 'M'
    { 0x7F, 0x04, 0x08, 0x10, 0x7F }, // 0x4E 'N'
    { 0x3E, 0x41, 0x41, 0x41, 0x3E }, // 0x4F 'O'
    { 0x7F, 0x09, 0x09, 0x09, 0x06 }, // 0x50 'P'
    { 0x3E, 0x41, 0x51, 0x21, 0x5E }, // 0x51 'Q'
    { 0x7F, 0x09, 0x19, 0x29, 0x46 }, // 0x52 'R'
    { 0x46, 0x49, 0x49, 0x49, 0x31 }, // 0x53 'S'
    { 0x01, 0x01, 0x7F, 0x01, 0x01 }, // 0x54 'T'
    { 0x3F, 0x40, 0x40, 0x40, 0x3F }, // 0x55 'U'
    { 0x1F, 0x20, 0x40, 0x20, 0x1F }, // 0x56 'V'
    { 0x7F, 0x20, 0x18, 0x20, 0x7F }, // 0x57 'W'
    { 0x63, 0x14, 0x08, 0x14, 0x63 }, // 0x58 'X'
    { 0x03, 0x04, 0x78, 0x04, 0x03 }, // 0x59 'Y'
    { 0x61, 0x51, 0x49, 0x45, 0x43 }, // 0x5A 'Z'
    { 0x00, 0x00, 0x7F, 0x41, 0x41 }, // 0x5B '['
    { 0x02, 0x04, 0x08, 0x10, 0x20 }, // 0x5C '\'
    { 0x41, 0x41, 0x7F, 0x00, 0x00 }, // 0x5D ']'
    { 0x04, 0x02, 0x01, 0x02, 0x04 }, // 0x5E '^'
    { 0x40, 0x40, 0x40, 0x40, 0x40 }, // 0x5F '_'
    { 0x00, 0x01, 0x02, 0x04, 0x00 }, // 0x60 '`'
    { 0x20, 0x54, 0x54, 0x54, 0x78 }, // 0x61 'a'
    { 0x7F, 0x48, 0x44, 0x44, 0x38 }, // 0x62 'b'
    { 0x38, 0x44, 0x44, 0x44, 0x20 }, // 0x63 'c'
    { 0x38, 0x44, 0x44, 0x48, 0x7F }, // 0x64 'd'
    { 0x38, 0x54, 0x54, 0x54, 0x18 }, // 0x65 'e'
    { 0x08, 0x7E, 0x09, 0x01, 0x02 }, // 0x66 'f'
    { 0x08, 0x14, 0x54, 0x54, 0x3C }, // 0x67 'g'
    { 0x7F, 0x08, 0x04, 0x04, 0x78 }, // 0x68 'h'
    { 0x00, 0x44, 0x7D, 0x40, 0x00 }, // 0x69 'i'
    { 0x20, 0x40, 0x44, 0x3D, 0x00 }, // 0x6A 'j'
    { 0x00, 0x7F, 0x10, 0x28, 0x44 }, // 0x6B 'k'
    { 0x00, 0x41, 0x7F, 0x40, 0x00 }, // 0x6C 'l'
    { 0x7C, 0x04, 0x18, 0x04, 0x78 }, // 0x6D 'm'
    { 0x7C, 0x08, 0x04, 0x04, 0x78 }, // 0x6E 'n'
    { 0x38, 0x44, 0x44, 0x44, 0x38 }, // 0x6F 'o'
    { 0x7C, 0x14, 0x14, 0x14, 0x08 }, // 0x70 'p'
    { 0x08, 0x14, 0x14, 0x18, 0x7C }, // 0x71 'q'
    { 0x7C, 0x08, 0x04, 0x04, 0x08 }, // 0x72 'r'
    { 0x48, 0x54, 0x54, 0x54, 0x20 }, // 0x73 's'
    { 0x04, 0x3F, 0x44, 0x40, 0x20 }, // 0x74 't'
    { 0x3C, 0x40, 0x40, 0x40, 0x7C }, // 0x75 'u'
    { 0x1C, 0x20, 0x40, 0x20, 0x1C }, // 0x76 'v'
    { 0x3C, 0x40, 0x30, 0x40, 0x3C }, // 0x77 'w'
    { 0x44, 0x28, 0x10, 0x28, 0x44 }, // 0x78 'x'
    { 0x0C, 0x50, 0x50, 0x50, 0x3C }, // 0x79 'y'
    { 0x44, 0x64, 0x54, 0x4C, 0x44 }, // 0x7A 'z'
    { 0x00, 0x08, 0x36, 0x41, 0x00 }, // 0x7B '{'
    { 0x00, 0x00, 0x7F, 0x00, 0x00 }, // 0x7C '|'
    { 0x00, 0x41, 0x36, 0x08, 0x00 }, // 0x7D '}'
    { 0x08, 0x08, 0x2A, 0x1C, 0x08 }, // 0x7E '~'
};

static i2c_master_dev_handle_t s_dev        = NULL;
static SemaphoreHandle_t       s_oled_mutex = NULL;
static char                    s_lines[3][22]; // cached text for lines 0–2

esp_err_t oled_init(void)
{
    // Give the display's power supply time to reach operating voltage.
    // SSD1306 datasheet recommends waiting before sending the first command.
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Initializing SSD1306 on SDA=%d SCL=%d", OLED_SDA_GPIO, OLED_SCL_GPIO);

    i2c_master_bus_config_t bus_cfg = {
        .clk_source               = I2C_CLK_SRC_DEFAULT,
        .i2c_port                 = -1,   // auto-select port
        .scl_io_num               = OLED_SCL_GPIO,
        .sda_io_num               = OLED_SDA_GPIO,
        .glitch_ignore_cnt        = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Probe both common SSD1306 addresses (SA0 pin tied to GND=0x3C, VCC=0x3D)
    uint16_t addr = 0;
    if (i2c_master_probe(bus, 0x3C, 50) == ESP_OK) {
        addr = 0x3C;
    } else if (i2c_master_probe(bus, 0x3D, 50) == ESP_OK) {
        addr = 0x3D;
        ESP_LOGW(TAG, "Found at 0x3D instead of 0x3C — update OLED_I2C_ADDR in config.h");
    } else {
        ESP_LOGE(TAG, "No SSD1306 found on I2C bus! Check wiring (SDA=%d SCL=%d) and power.",
                 OLED_SDA_GPIO, OLED_SCL_GPIO);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "SSD1306 detected at 0x%02X", addr);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = OLED_I2C_FREQ_HZ,
    };
    ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // SSD1306 initialization sequence for 128×32.
    // The leading 0x00 is the I2C control byte: Co=0, D/C=0 → command stream.
    static const uint8_t init[] = {
        0x00,       // control byte: all following bytes are commands
        0xAE,       // display off
        0xD5, 0x80, // clock divide ratio / oscillator frequency
        0xA8, 0x1F, // multiplex ratio: 32 rows (0x1F = 31)
        0xD3, 0x00, // display offset: 0
        0x40,       // display start line: 0
        0x8D, 0x14, // charge pump: enable (required without external Vcc)
        0x20, 0x00, // memory addressing mode: horizontal (auto page-wrap)
        0xA1,       // segment remap: col 127 → SEG0 (correct left/right orientation)
        0xC8,       // COM scan direction: remapped (correct up/down orientation)
        0xDA, 0x02, // COM pins hardware config: sequential, no remap (for 32px height)
        0x81, 0xCF, // contrast
        0xD9, 0xF1, // pre-charge period
        0xDB, 0x40, // VCOMH deselect level
        0xA4,       // display follows RAM content (not "all on")
        0xA6,       // normal display (not inverted)
        0xAF,       // display on
    };
    ret = i2c_master_transmit(s_dev, init, sizeof(init), -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Init sequence failed: %s", esp_err_to_name(ret));
        return ret;
    }

    oled_clear();
    s_oled_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "SSD1306 128x32 ready");
    return ESP_OK;
}

void oled_clear(void)
{
    // Write 128 zero bytes to each of the 4 pages
    for (uint8_t page = 0; page < 4; page++) {
        // Set address window: columns 0-127, this page only
        uint8_t addr[] = { 0x00, 0x21, 0, 127, 0x22, page, page };
        i2c_master_transmit(s_dev, addr, sizeof(addr), -1);

        // 0x40 = data stream control byte, followed by 128 zero pixel-columns
        uint8_t data[129];
        data[0] = 0x40;
        memset(data + 1, 0x00, 128);
        i2c_master_transmit(s_dev, data, sizeof(data), -1);
    }
}

void oled_puts(uint8_t page, uint8_t col, const char *text)
{
    if (!s_dev || !text) return;

    // Set address window: columns col-127, single page
    uint8_t addr[] = { 0x00, 0x21, col, 127, 0x22, page, page };
    i2c_master_transmit(s_dev, addr, sizeof(addr), -1);

    // Pack all character glyphs into one I2C data transaction.
    // Each character: 5 font bytes + 1 zero gap byte = 6 pixel-columns.
    // Max chars at col=0: 128/6 = 21 chars.
    uint8_t buf[129]; // 1 control byte + up to 128 pixel columns
    buf[0] = 0x40;    // data stream
    int len = 1;

    while (*text && len + 6 <= (int)sizeof(buf)) {
        uint8_t c = (uint8_t)*text++;
        if (c < 0x20 || c > 0x7E) c = '?';
        const uint8_t *glyph = s_font[c - 0x20];
        for (int i = 0; i < 5; i++) buf[len++] = glyph[i];
        buf[len++] = 0x00; // 1-pixel inter-character gap
    }

    if (len > 1)
        i2c_master_transmit(s_dev, buf, len, -1);
}

void oled_puts_large(uint8_t page, uint8_t col, const char *text)
{
    if (!s_dev || !text || page > 2) return;

    // Map 5 input font columns to 6 output columns (col 2 duplicated to widen
    // the centre stroke), then add 2 zero margin columns → 8 total per character.
    // Row doubling: each of the 7 source rows becomes 2 consecutive output rows,
    // giving a 14-row glyph centred in the 16-row (2-page) character cell.
    static const uint8_t s_col_map[6] = {0, 1, 2, 2, 3, 4};

    int max_chars = (128 - col) / 8; // 16 chars max at col=0
    uint8_t top_buf[129];            // control byte + up to 128 pixel columns
    uint8_t bot_buf[129];
    top_buf[0] = bot_buf[0] = 0x40; // I2C data stream control byte
    int chars = 0;

    while (*text && chars < max_chars) {
        uint8_t c = (uint8_t)*text++;
        if (c < 0x20 || c > 0x7E) c = '?';
        const uint8_t *g = s_font[c - 0x20];

        for (int oc = 0; oc < 8; oc++) {
            uint8_t b = (oc < 6) ? g[s_col_map[oc]] : 0;

            // Input rows 0-3 → output rows 0-7 (top page, bit 0 = topmost row)
            uint8_t top = 0;
            for (int r = 0; r < 4; r++) {
                if (b & (1u << r)) top |= (3u << (r * 2));
            }
            // Input rows 4-6 → output rows 8-13 (bot page); rows 14-15 stay 0
            uint8_t bot = 0;
            for (int r = 0; r < 3; r++) {
                if (b & (1u << (r + 4))) bot |= (3u << (r * 2));
            }
            top_buf[1 + chars * 8 + oc] = top;
            bot_buf[1 + chars * 8 + oc] = bot;
        }
        chars++;
    }

    if (chars == 0) return;
    int bytes = chars * 8;
    uint8_t ec = (uint8_t)(col + bytes - 1);

    uint8_t at[] = {0x00, 0x21, col, ec, 0x22, page,          page};
    uint8_t ab[] = {0x00, 0x21, col, ec, 0x22, page + 1u, page + 1u};
    i2c_master_transmit(s_dev, at, sizeof(at), -1);
    i2c_master_transmit(s_dev, top_buf, bytes + 1, -1);
    i2c_master_transmit(s_dev, ab, sizeof(ab), -1);
    i2c_master_transmit(s_dev, bot_buf, bytes + 1, -1);
}

// Build all 4 page buffers from the s_lines cache and write them to the display.
//
// Cross-page vertical layout (32px total):
//   rows 0–6    : line 0  → page 0, bits 0–6  (no shift)
//   rows 7–11   : gap 5px (blank)
//   rows 12–18  : line 1  → page 1 bits 4–7, page 2 bits 0–2
//   rows 19–23  : gap 5px (blank)
//   rows 24–30  : line 2  → page 3, bits 0–6  (no shift)
//   row  31     : blank   (font bit 7 is always 0)
//
// Called with s_oled_mutex already held.
static void render_display(void)
{
    uint8_t page[4][129];
    for (int p = 0; p < 4; p++) {
        page[p][0] = 0x40; // I2C data-stream control byte
        memset(page[p] + 1, 0, 128);
    }

    for (int ln = 0; ln < 3; ln++) {
        const char *text = s_lines[ln];
        int col = 1; // buffer index (col 1 = pixel column 0)

        for (const char *p = text; *p && col + 6 <= 129; p++) {
            uint8_t c = (uint8_t)*p;
            if (c < 0x20 || c > 0x7E) c = '?';
            const uint8_t *glyph = s_font[c - 0x20];

            for (int gc = 0; gc < 5; gc++, col++) {
                uint8_t b = glyph[gc];
                switch (ln) {
                case 0:
                    // rows 0–6: page 0 bits 0–6 (no shift)
                    page[0][col] |= b;
                    break;
                case 1:
                    // rows 12–18: page 1 bits 4–7 + page 2 bits 0–2
                    page[1][col] |= (uint8_t)((b & 0x0F) << 4);
                    page[2][col] |= (uint8_t)((b >> 4) & 0x07);
                    break;
                case 2:
                    // rows 24–30: page 3 bits 0–6 (no shift)
                    page[3][col] |= b;
                    break;
                }
            }
            col++; // inter-character gap (already zero)
        }
    }

    for (uint8_t p = 0; p < 4; p++) {
        uint8_t addr[] = {0x00, 0x21, 0, 127, 0x22, p, p};
        i2c_master_transmit(s_dev, addr, sizeof(addr), -1);
        i2c_master_transmit(s_dev, page[p], 129, -1);
    }
}

void oled_set_line(uint8_t line, const char *text)
{
    if (!s_dev || line > 2) return;
    if (!text) text = "";

    if (s_oled_mutex) xSemaphoreTake(s_oled_mutex, portMAX_DELAY);

    strncpy(s_lines[line], text, sizeof(s_lines[line]) - 1);
    s_lines[line][sizeof(s_lines[line]) - 1] = '\0';
    render_display();

    if (s_oled_mutex) xSemaphoreGive(s_oled_mutex);
}
