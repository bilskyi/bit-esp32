#include "ssd1306.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "ssd1306";

// Every transaction is prefixed with a control byte saying what follows.
#define CTRL_CMD 0x00
#define CTRL_DATA 0x40

#define I2C_TIMEOUT_MS 200

// Panel setup, straight out of the datasheet's power-on sequence.
//
// The two that are easy to get wrong and hard to diagnose:
//   0x8D 0x14  the charge pump. Without it the panel initialises, acknowledges
//              every byte, and stays completely dark - there is no rail to
//              light the pixels with.
//   0xA1/0xC8  segment remap and reverse COM scan. Omit them and the image is
//              mirrored in both axes, which on a symmetrical pair of eyes is
//              almost invisible until the first blink goes the wrong way.
static const uint8_t INIT_SEQUENCE[] = {
    0xAE,        // display off while we reconfigure
    0xD5, 0x80,  // clock: divide by 1, oscillator at its default
    0xA8, 0x3F,  // multiplex ratio: 64 rows
    0xD3, 0x00,  // no vertical offset
    0x40,        // start line 0
    0x8D, 0x14,  // charge pump on
    0x20, 0x00,  // horizontal addressing, so a page is one run of 128 bytes
    0xA1,        // segment remap: column 127 is SEG0
    0xC8,        // COM scan from the bottom up
    0xDA, 0x12,  // alternate COM pin layout, which 128x64 modules use
    0x81, 0xCF,  // contrast
    0xD9, 0xF1,  // pre-charge
    0xDB, 0x40,  // VCOMH deselect
    0xA4,        // follow RAM rather than forcing every pixel on
    0xA6,        // not inverted
    0x2E,        // scrolling off
    0xAF,        // display on
};

static esp_err_t write_cmds(ssd1306_t *d, const uint8_t *cmds, size_t n) {
    // Command bytes go one per transaction with their control byte. Slower
    // than streaming them, and this only ever runs at startup.
    for (size_t i = 0; i < n; i++) {
        const uint8_t pair[2] = {CTRL_CMD, cmds[i]};
        const esp_err_t err = i2c_master_transmit(d->dev, pair, 2, I2C_TIMEOUT_MS);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

// Points the panel's auto-incrementing pointer at a range of pages, full width.
static esp_err_t set_window(ssd1306_t *d, uint8_t page_start, uint8_t page_end) {
    const uint8_t cmds[] = {
        0x21, 0, FACE_W - 1,          // column range
        0x22, page_start, page_end,   // page range
    };
    return write_cmds(d, cmds, sizeof(cmds));
}

esp_err_t ssd1306_init(ssd1306_t *d, gpio_num_t sda, gpio_num_t scl, uint32_t hz) {
    memset(d, 0, sizeof(*d));

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,  // let the driver pick a free port
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &d->bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no I2C bus on SDA %d / SCL %d: %s", (int)sda, (int)scl,
                 esp_err_to_name(err));
        d->bus = NULL;
        return err;
    }

    // Modules are strapped to one address or the other and the silkscreen
    // rarely says which, so ask both before giving up.
    const uint8_t candidates[] = {SSD1306_ADDR_PRIMARY, SSD1306_ADDR_SECONDARY};
    d->addr = 0;
    for (size_t i = 0; i < sizeof(candidates); i++) {
        if (i2c_master_probe(d->bus, candidates[i], 100) == ESP_OK) {
            d->addr = candidates[i];
            break;
        }
    }
    if (d->addr == 0) {
        ESP_LOGW(TAG, "nothing answers at 0x3C or 0x3D on SDA %d / SCL %d",
                 (int)sda, (int)scl);
        i2c_del_master_bus(d->bus);
        d->bus = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = d->addr,
        .scl_speed_hz = hz,
    };
    err = i2c_master_bus_add_device(d->bus, &dev_cfg, &d->dev);
    if (err != ESP_OK) {
        i2c_del_master_bus(d->bus);
        d->bus = NULL;
        return err;
    }

    err = write_cmds(d, INIT_SEQUENCE, sizeof(INIT_SEQUENCE));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init sequence failed: %s", esp_err_to_name(err));
        ssd1306_deinit(d);
        return err;
    }

    d->ready = true;
    d->shadow_valid = false;
    ESP_LOGI(TAG, "panel at 0x%02X, %ux%u, %lu kHz", d->addr, FACE_W, FACE_H,
             (unsigned long)(hz / 1000));

    return ssd1306_clear(d);
}

esp_err_t ssd1306_flush(ssd1306_t *d, const uint8_t *fb) {
    if (!d->ready) return ESP_ERR_INVALID_STATE;

    int first = 0, last = FACE_PAGES - 1;

    if (d->shadow_valid) {
        // Send one contiguous run covering everything that changed. Two runs
        // would save a little traffic when only the top and bottom moved, but
        // each run costs its own six command transactions, and the eyes are
        // one connected shape - the pages between two changed pages have
        // almost always changed too.
        first = FACE_PAGES;
        last = -1;
        for (int p = 0; p < FACE_PAGES; p++) {
            if (memcmp(&d->shadow[p * FACE_W], &fb[p * FACE_W], FACE_W) != 0) {
                if (p < first) first = p;
                last = p;
            }
        }
        if (last < 0) return ESP_OK;  // nothing moved this frame
    }

    const esp_err_t err = set_window(d, (uint8_t)first, (uint8_t)last);
    if (err != ESP_OK) return err;

    const size_t n = (size_t)(last - first + 1) * FACE_W;
    d->scratch[0] = CTRL_DATA;
    memcpy(&d->scratch[1], &fb[first * FACE_W], n);

    const esp_err_t werr = i2c_master_transmit(d->dev, d->scratch, n + 1, I2C_TIMEOUT_MS);
    if (werr != ESP_OK) {
        // The next flush must not trust a shadow the panel may not have.
        d->shadow_valid = false;
        return werr;
    }

    memcpy(d->shadow, fb, FACE_FB_BYTES);
    d->shadow_valid = true;
    return ESP_OK;
}

esp_err_t ssd1306_clear(ssd1306_t *d) {
    if (!d->ready) return ESP_ERR_INVALID_STATE;

    static const uint8_t blank[FACE_FB_BYTES] = {0};
    d->shadow_valid = false;  // force the whole frame out
    return ssd1306_flush(d, blank);
}

esp_err_t ssd1306_set_contrast(ssd1306_t *d, uint8_t contrast) {
    if (!d->ready) return ESP_ERR_INVALID_STATE;
    const uint8_t cmds[] = {0x81, contrast};
    return write_cmds(d, cmds, sizeof(cmds));
}

esp_err_t ssd1306_display(ssd1306_t *d, bool on) {
    if (!d->ready) return ESP_ERR_INVALID_STATE;
    const uint8_t cmd = on ? 0xAF : 0xAE;
    return write_cmds(d, &cmd, 1);
}

void ssd1306_deinit(ssd1306_t *d) {
    if (d->dev) {
        i2c_master_bus_rm_device(d->dev);
        d->dev = NULL;
    }
    if (d->bus) {
        i2c_del_master_bus(d->bus);
        d->bus = NULL;
    }
    d->ready = false;
    d->shadow_valid = false;
}
