// Step 2 of the build order: capture only.
//
// Reads the INMP441 over I2S and prints a level meter to the serial console.
// No WiFi, no WebSocket, no playback. The entire point is to prove the
// microphone works before anything can hide a failure behind a network.
//
// A silent microphone is the most common failure on this board, and it is
// much harder to diagnose through a WebSocket than through a printed number.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Mic and amp share BCLK and WS; this sketch only drives the RX half.
#define PIN_BCLK GPIO_NUM_4
#define PIN_WS GPIO_NUM_5
#define PIN_DIN GPIO_NUM_6
#define PIN_BUTTON GPIO_NUM_3  // push-to-talk, wired to GND, active low

#define SAMPLE_RATE 16000
#define BLOCK_SAMPLES 1024              // 64 ms per block at 16 kHz
#define PRINT_EVERY 4                   // report roughly four times a second
#define SILENCE_BLOCKS (PRINT_EVERY * 8)  // ~2 s of nothing before complaining

static const char *TAG = "capture";

// Static rather than stack: 4 KB + 2 KB is a lot to ask of a task stack, and
// the whole buffer budget for this project is about 40 KB.
static int32_t s_raw[BLOCK_SAMPLES];
static int16_t s_pcm[BLOCK_SAMPLES];

// Cascaded high-pass, three one-pole sections at ~100 Hz.
//
// Measured on this hardware: the INMP441's output is dominated by 0.6-4 Hz
// drift carrying ~89% of all energy, at roughly a quarter of full scale. A
// single pole at 20 Hz only attenuates 1 Hz by ~26 dB, which left speech at
// 0.09% of total energy - inaudible to Whisper, which returned hallucinated
// text. Three poles at 100 Hz put that same drift ~120 dB down and lift the
// speech band to ~55%.
//
// Cost at the top end is negligible: about -1.4 dB at 300 Hz. Voice
// fundamentals below 100 Hz are attenuated, but intelligibility lives in the
// formants above 300 Hz, which is also all the STT needs.
#define HP_STAGES 3
#define HP_A 31226  // 0.9529 in Q15 -> corner near 100 Hz at 16 kHz

static int32_t s_hp_x1[HP_STAGES];
static int32_t s_hp_y1[HP_STAGES];

static inline int32_t high_pass(int32_t x) {
    for (int s = 0; s < HP_STAGES; s++) {
        // int64 intermediate: y1 * HP_A overflows int32 at 24-bit amplitudes.
        int64_t y = (int64_t)x - (int64_t)s_hp_x1[s] +
                    (((int64_t)s_hp_y1[s] * HP_A) >> 15);
        s_hp_x1[s] = x;
        if (y > INT32_MAX) y = INT32_MAX;
        if (y < INT32_MIN) y = INT32_MIN;
        s_hp_y1[s] = (int32_t)y;
        x = s_hp_y1[s];
    }
    return x;
}

static i2s_chan_handle_t rx_channel(void) {
    i2s_chan_handle_t rx = NULL;

    // The C3 has exactly one I2S controller, so there is no choice to make.
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        // 32-bit slots even though the mic carries 24 bits: the INMP441 sends
        // 24-bit data left-aligned inside a 32-bit slot, so the slot width has
        // to match the wire, not the payload.
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,  // the INMP441 makes its own clock
                .bclk = PIN_BCLK,
                .ws = PIN_WS,
                .dout = I2S_GPIO_UNUSED,  // capture only
                .din = PIN_DIN,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };

    // L/R is tied to GND on the mic, which puts it in the left slot. Getting
    // this wrong is the classic "reads all zeros" bug, so set it explicitly
    // instead of trusting the default.
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx));
    return rx;
}

static void button_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,  // switch pulls to GND when pressed
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

void app_main(void) {
    ESP_LOGI(TAG, "capture-only build: %d Hz, %d-sample blocks", SAMPLE_RATE,
             BLOCK_SAMPLES);
    ESP_LOGI(TAG, "bclk=%d ws=%d din=%d button=%d", PIN_BCLK, PIN_WS, PIN_DIN,
             PIN_BUTTON);
    ESP_LOGI(TAG, "speak at the mic; rms should rise well above its idle floor");

    button_init();
    i2s_chan_handle_t rx = rx_channel();

    unsigned block = 0;
    unsigned quiet_blocks = 0;
    int64_t sum_sq = 0;
    int32_t peak = 0;
    int64_t dc_sum = 0;
    int64_t raw_dc_sum = 0;  // offset before filtering, for diagnosis

    while (true) {
        size_t got = 0;
        esp_err_t err =
            i2s_channel_read(rx, s_raw, sizeof(s_raw), &got, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s read failed: %s", esp_err_to_name(err));
            continue;
        }

        const size_t n = got / sizeof(int32_t);
        for (size_t i = 0; i < n; i++) {
            // The mic sends 24 bits left-aligned in the slot, so the low byte
            // is always zero. Filter in the 24-bit domain to keep those extra
            // bits of headroom while the offset is being removed.
            const int32_t s24 = s_raw[i] >> 8;
            raw_dc_sum += s24;

            const int32_t filtered = high_pass(s24);

            // Down to int16 only after filtering. This is the same width the
            // networked firmware sends, and what keeps the link at 256 kbit/s
            // instead of four times that.
            const int16_t s = (int16_t)(filtered >> 8);
            s_pcm[i] = s;

            sum_sq += (int64_t)s * (int64_t)s;
            dc_sum += s;
            const int32_t mag = s < 0 ? -(int32_t)s : (int32_t)s;
            if (mag > peak) peak = mag;
        }

        if (++block < PRINT_EVERY) continue;

        const size_t total = (size_t)block * n;
        const double rms = sqrt((double)sum_sq / (double)total);
        const double dbfs = rms > 0.0 ? 20.0 * log10(rms / 32768.0) : -120.0;
        const int dc = (int)(dc_sum / (int64_t)total);
        const bool pressed = gpio_get_level(PIN_BUTTON) == 0;

        // "dc" is the residual after filtering and should sit near zero.
        // "raw" is the mic's own offset, scaled to int16 for comparison: it is
        // expected to be large and to wander, and is shown only to prove the
        // blocker is doing its job.
        const int raw_dc = (int)((raw_dc_sum / (int64_t)total) >> 8);

        printf("rms %6.0f  peak %6ld  dc %5d  raw %6d  %6.1f dBFS  btn %s\n",
               rms, (long)peak, dc, raw_dc, dbfs, pressed ? "DOWN" : "up");

        // All-zero samples mean the data line is dead. That is a wiring or
        // slot-mask problem, never a quiet room: even silence carries noise.
        if (peak == 0) {
            if (++quiet_blocks * PRINT_EVERY >= SILENCE_BLOCKS) {
                ESP_LOGW(TAG,
                         "every sample is zero - check SD->GPIO%d, SCK->GPIO%d, "
                         "WS->GPIO%d, L/R->GND and 3V3",
                         PIN_DIN, PIN_BCLK, PIN_WS);
                quiet_blocks = 0;
            }
        } else {
            quiet_blocks = 0;
        }

        block = 0;
        sum_sq = 0;
        peak = 0;
        dc_sum = 0;
        raw_dc_sum = 0;
    }
}
