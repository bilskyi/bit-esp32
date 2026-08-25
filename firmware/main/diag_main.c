// Diagnostic sketch. Not part of the build order.
//
// capture prints a level meter, which tells you a number is moving but not
// whether the bits underneath it are framed correctly. This prints the raw
// 32-bit I2S words so alignment can be read directly.
//
// What to look for, for an INMP441 sending 24-bit data left-aligned in a
// 32-bit slot:
//
//   - the low byte of every word should be 0x00 (24 bits in a 32-bit slot)
//   - words should straddle zero, not sit at one extreme
//   - "zero-low-byte" should be 100%; anything less means the frame is
//     slipping and the sample boundary is not where we think it is

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_BCLK GPIO_NUM_4
#define PIN_WS GPIO_NUM_5
#define PIN_DIN GPIO_NUM_6

#define SAMPLE_RATE 16000
#define BLOCK_SAMPLES 512

static const char *TAG = "diag";
static int32_t s_raw[BLOCK_SAMPLES];

void app_main(void) {
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = PIN_BCLK,
                .ws = PIN_WS,
                .dout = I2S_GPIO_UNUSED,
                .din = PIN_DIN,
                .invert_flags = {false, false, false},
            },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx));

    ESP_LOGI(TAG, "raw 32-bit I2S words; low byte should be 00 for a 24-bit mic");

    while (true) {
        size_t got = 0;
        if (i2s_channel_read(rx, s_raw, sizeof(s_raw), &got, portMAX_DELAY) !=
            ESP_OK) {
            continue;
        }
        const size_t n = got / sizeof(int32_t);

        unsigned zero_low = 0;
        int32_t lo = INT32_MAX, hi = INT32_MIN;
        for (size_t i = 0; i < n; i++) {
            if ((s_raw[i] & 0xFF) == 0) zero_low++;
            if (s_raw[i] < lo) lo = s_raw[i];
            if (s_raw[i] > hi) hi = s_raw[i];
        }

        printf("raw:");
        for (int i = 0; i < 8 && (size_t)i < n; i++) printf(" %08lx", (unsigned long)s_raw[i]);
        printf("\n");
        printf("  zero-low-byte %u/%u (%u%%)  min %ld  max %ld  span %ld\n",
               zero_low, (unsigned)n, (unsigned)(zero_low * 100 / n), (long)lo,
               (long)hi, (long)(hi - lo));

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
