// Step 3 of the build order: playback and mute discipline.
//
// Plays a 440 Hz tone through the MAX98357A, then silences it, in a loop.
// No microphone, no WiFi. Two things are being proven here:
//
//   1. The amp is wired and powered correctly and makes a clean tone.
//   2. Playback is switched with the shutdown pin alone. The I2S channel is
//      initialised exactly once, at boot, and never touched again. Re-init
//      between utterances is what makes the audible pop the spec warns about,
//      so this sketch deliberately never does it.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Mic and amp share BCLK and WS; this sketch only drives the TX half.
#define PIN_BCLK GPIO_NUM_4
#define PIN_WS GPIO_NUM_5
#define PIN_DOUT GPIO_NUM_7
#define PIN_MUTE GPIO_NUM_10  // MAX98357A SD: low shuts the amp down

#define SAMPLE_RATE 16000
#define TONE_HZ 440
#define BLOCK_SAMPLES 1024
#define TONE_MS 1000
#define GAP_MS 1000

// About -12 dBFS. Loud enough to hear across a room, quiet enough that a
// wiring mistake does not arrive at full scale.
#define AMPLITUDE 8000.0

// The default DMA ring holds roughly 90 ms at this rate. Muting before it has
// drained truncates the tail; this waits it out with margin.
#define DRAIN_MS 150

static const char *TAG = "playback";

static int16_t s_block[BLOCK_SAMPLES];
static double s_phase = 0.0;

static i2s_chan_handle_t tx_channel(void) {
    i2s_chan_handle_t tx = NULL;

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // Second argument is the TX handle; RX stays null in this sketch.
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        // 16-bit here, unlike capture: what we send is already int16, which is
        // the format the server streams and the format the amp wants.
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = PIN_BCLK,
                .ws = PIN_WS,
                .dout = PIN_DOUT,
                .din = I2S_GPIO_UNUSED,  // playback only
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx));
    return tx;
}

static void mute_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_MUTE,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    // Muted before the first sample exists, and the resting state thereafter.
    ESP_ERROR_CHECK(gpio_set_level(PIN_MUTE, 0));
}

static void amp_enable(bool on) { ESP_ERROR_CHECK(gpio_set_level(PIN_MUTE, on ? 1 : 0)); }

// Fills one block of sine, ramping gain from g0 to g1 across it. The ramp is
// what keeps the start and end of a tone from clicking: a waveform that begins
// at full amplitude is a step, and a step is a click.
static void fill_tone(int16_t *buf, size_t n, float g0, float g1) {
    const double step = 2.0 * M_PI * (double)TONE_HZ / (double)SAMPLE_RATE;
    for (size_t i = 0; i < n; i++) {
        const float g = g0 + (g1 - g0) * ((float)i / (float)n);
        buf[i] = (int16_t)(AMPLITUDE * (double)g * sin(s_phase));
        s_phase += step;
        if (s_phase > 2.0 * M_PI) s_phase -= 2.0 * M_PI;
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "playback build: %d Hz tone at %d Hz sample rate", TONE_HZ,
             SAMPLE_RATE);
    ESP_LOGI(TAG, "bclk=%d ws=%d dout=%d mute=%d", PIN_BCLK, PIN_WS, PIN_DOUT,
             PIN_MUTE);
    ESP_LOGI(TAG, "the amp needs 5V, not 3V3, and peaks near 700 mA");

    mute_init();

    // Initialised once. Every state change below is a pin, never a re-init.
    i2s_chan_handle_t tx = tx_channel();

    const int blocks = (SAMPLE_RATE * TONE_MS / 1000) / BLOCK_SAMPLES;

    while (true) {
        printf("tone  (mute pin high)\n");
        amp_enable(true);

        for (int b = 0; b < blocks; b++) {
            const float g0 = (b == 0) ? 0.0f : 1.0f;
            const float g1 = (b == blocks - 1) ? 0.0f : 1.0f;
            fill_tone(s_block, BLOCK_SAMPLES, g0, g1);

            size_t written = 0;
            esp_err_t err = i2s_channel_write(tx, s_block, sizeof(s_block),
                                              &written, portMAX_DELAY);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(err));
                break;
            }
        }

        // Let the DMA ring empty before cutting the amp, or the tail is lost.
        vTaskDelay(pdMS_TO_TICKS(DRAIN_MS));

        printf("silence (mute pin low)\n");
        amp_enable(false);
        vTaskDelay(pdMS_TO_TICKS(GAP_MS));
    }
}
