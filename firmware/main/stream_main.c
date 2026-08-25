// Bench sketch: stream microphone PCM to the host over USB.
//
// Not part of the build order. It exists to answer one question before any
// networking is written: is the audio this microphone produces good enough for
// Whisper to transcribe? The device has no WiFi yet, so the host stands in for
// the WebSocket - it captures the same int16 stream the networked firmware
// will eventually send, writes a WAV, and posts it to the real STT provider.
//
// Frame format, repeating forever:
//
//     A5 5A A5 5A                 4-byte sync magic
//     <BLOCK_SAMPLES * int16>     little-endian mono PCM at 16 kHz
//
// The magic exists so the host can find a sample boundary in a byte stream it
// joined halfway through. Without it a one-byte offset turns speech into noise.
//
// Gain is a build option: -DPCM_SHIFT=6 keeps two more bits of the 24-bit
// source than the default 8, which is 12 dB louder without amplifying
// quantisation noise the way a post-shift multiply would.

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2s_std.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_BCLK GPIO_NUM_4
#define PIN_WS GPIO_NUM_5
#define PIN_DIN GPIO_NUM_6

#define SAMPLE_RATE 16000
#define BLOCK_SAMPLES 1024

#ifndef PCM_SHIFT
#define PCM_SHIFT 4
#endif

static const uint8_t MAGIC[4] = {0xA5, 0x5A, 0xA5, 0x5A};

// Stereo frames: the INMP441 needs BCLK of at least 512 kHz, and a single
// activated slot at 16 kHz x 32 bits lands exactly on that limit, where the
// mic returns low-frequency garbage instead of audio. Clocking two slots puts
// BCLK at 1.024 MHz, well inside spec. The right slot is silent (L/R is tied
// to GND) and is discarded.
static int32_t s_raw[BLOCK_SAMPLES * 2];
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

void app_main(void) {
    // Binary on the wire: any stray log line would be parsed as audio.
    esp_log_level_set("*", ESP_LOG_NONE);

    // Write through the USB Serial/JTAG driver rather than stdio. The console
    // is configured for CRLF translation, which would rewrite every 0x0A byte
    // in the PCM and corrupt the stream.
    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ucfg.tx_buffer_size = 4096;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&ucfg));

    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
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

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx));

    while (true) {
        size_t got = 0;
        if (i2s_channel_read(rx, s_raw, sizeof(s_raw), &got, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        // Two slots per frame; the mic occupies the left one.
        const size_t n = got / sizeof(int32_t) / 2;

        for (size_t i = 0; i < n; i++) {
            int32_t v = high_pass(s_raw[i * 2] >> 8) >> PCM_SHIFT;
            // Clamp: at PCM_SHIFT < 8 a loud sound can exceed int16.
            if (v > INT16_MAX) v = INT16_MAX;
            if (v < INT16_MIN) v = INT16_MIN;
            s_pcm[i] = (int16_t)v;
        }

        usb_serial_jtag_write_bytes(MAGIC, sizeof(MAGIC), portMAX_DELAY);
        usb_serial_jtag_write_bytes(s_pcm, n * sizeof(int16_t), portMAX_DELAY);
    }
}
