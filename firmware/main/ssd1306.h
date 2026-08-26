// The 0.96" SSD1306 over I2C: init, probe, and a flush that only sends the
// pages that changed.
//
// Written rather than pulled in, for the same reason the ADPCM codec was: it
// is a command table and one transaction, the dependency would cost more than
// the code, and having it here means the probe can answer the question the
// firmware actually needs answered - is there a panel on this bus at all.
//
// The framebuffer it takes is face.c's, already in the panel's page layout.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#include "face.h"

// Both addresses an SSD1306 module is ever strapped to.
#define SSD1306_ADDR_PRIMARY 0x3C
#define SSD1306_ADDR_SECONDARY 0x3D

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    uint8_t addr;
    bool ready;

    // What the panel is currently showing, so a flush can skip pages that did
    // not change. Asleep the eyes occupy one page, and skipping the other
    // seven takes the transfer from 23 ms to 3.
    uint8_t shadow[FACE_FB_BYTES];
    bool shadow_valid;

    // 0x40 (a data stream) followed by up to a whole frame.
    uint8_t scratch[1 + FACE_FB_BYTES];
} ssd1306_t;

// Brings up the bus, looks for a panel at either address, and initialises it.
//
// Returns ESP_ERR_NOT_FOUND when nothing acknowledges. That is not a failure
// worth stopping for: the device has to keep working with no display attached,
// so the caller logs it and carries on.
esp_err_t ssd1306_init(ssd1306_t *d, gpio_num_t sda, gpio_num_t scl, uint32_t hz);

// Sends the pages that differ from what the panel already has.
esp_err_t ssd1306_flush(ssd1306_t *d, const uint8_t *fb);

esp_err_t ssd1306_clear(ssd1306_t *d);
esp_err_t ssd1306_set_contrast(ssd1306_t *d, uint8_t contrast);
esp_err_t ssd1306_display(ssd1306_t *d, bool on);
void ssd1306_deinit(ssd1306_t *d);
