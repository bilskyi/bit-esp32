// Step 4 of the build order: the whole device.
//
// Button held -> microphone streams to the server over a WebSocket. Button
// released -> the server transcribes, thinks, and streams a spoken reply back,
// which plays through the amplifier. Three tasks, as the spec lays out:
//
//   audio_in  - reads I2S, filters, pushes int16 into the mic buffer
//   net       - drains the mic buffer to the socket, feeds replies to the play
//               buffer, and owns the button and the control protocol
//   audio_out - plays the reply, and owns the mute pin
//
// One I2S controller, one clock pair, shared by microphone and amplifier.
// The channel is created once at boot and never re-initialised: playback is
// switched with the shutdown pin alone, because re-init is what pops.
//
// The slot width is 32 bits in BOTH directions. The microphone needs it (24
// bits left-aligned), and full duplex cannot run two different slot formats on
// one shared clock. The amplifier is happy to take a 16-bit sample sitting in
// the top half of a 32-bit slot, so that is what playback writes.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "secrets.h"

#define PIN_BCLK GPIO_NUM_4
#define PIN_WS GPIO_NUM_5
#define PIN_DIN GPIO_NUM_6
#define PIN_DOUT GPIO_NUM_7
#define PIN_MUTE GPIO_NUM_10
#define PIN_BUTTON GPIO_NUM_3

#define SAMPLE_RATE 16000
#define BLOCK_SAMPLES 512

// Gain, as bits kept from the 24-bit source.
//
// Measured on this hardware: shifting 4 (16x) drove speech to rms -14 dBFS
// with 1.1% of samples hard-clipped, and the STT returned garbage in the
// wrong alphabet. Shifting 6 (4x) lands near -26 dBFS with 12 dB of headroom,
// which is a healthy level for Whisper and leaves room for a raised voice.
#define PCM_SHIFT 6

// ~27 KB of buffers all told, inside the ~40 KB the spec allows.
#define MIC_BUFFER_BYTES 16384   // ~0.5 s of headroom on upload
#define PLAY_BUFFER_BYTES 24576  // ~0.77 s of jitter absorption
// Do not open the amplifier until this much reply is in hand. The spec budgets
// 150 ms of playback buffer; starting earlier means the first word stutters
// while the network catches up.
#define PREBUFFER_BYTES 4800     // 150 ms at 16 kHz mono 16-bit

#define DEBOUNCE_MS 25
// Never block forever on a send. The button is polled in the same loop, so an
// unbounded wait means a release is never noticed and the device stays stuck
// in "listening" until it is reset - which is exactly what happened.
#define SEND_TIMEOUT pdMS_TO_TICKS(400)
// Longest the device will wait on the server before re-arming the button.
#define STUCK_TIMEOUT_MS 20000
// How long the socket task may wait for room in the play buffer.
#define PLAY_SEND_TIMEOUT pdMS_TO_TICKS(3000)
#define DRAIN_MS 150  // let the DMA ring empty before cutting the amp

static const char *TAG = "voice";

static StreamBufferHandle_t s_mic_buf;
static StreamBufferHandle_t s_play_buf;
static EventGroupHandle_t s_wifi_events;
static esp_websocket_client_handle_t s_ws;

#define WIFI_CONNECTED_BIT BIT0

typedef enum { ST_IDLE, ST_LISTENING, ST_THINKING, ST_SPEAKING } state_t;
static volatile state_t s_state = ST_IDLE;
static volatile bool s_reply_finished = false;
static volatile uint32_t s_dropped_blocks = 0;
static volatile uint32_t s_sent_bytes = 0;
static volatile uint32_t s_send_failures = 0;
static volatile uint32_t s_play_dropped = 0;

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;

// ---------------------------------------------------------------- filtering

// Three one-pole sections at ~100 Hz. The INMP441's output is dominated by
// 0.6-4 Hz drift at roughly a quarter of full scale; a single pole at 20 Hz
// left speech at 0.09% of total energy and the STT returned hallucinated text.
#define HP_STAGES 3
#define HP_A 31226

static int32_t s_hp_x1[HP_STAGES];
static int32_t s_hp_y1[HP_STAGES];

static inline int32_t high_pass(int32_t x) {
    for (int s = 0; s < HP_STAGES; s++) {
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

// --------------------------------------------------------------------- i2s

static void audio_init(void) {
    gpio_config_t mute = {
        .pin_bit_mask = 1ULL << PIN_MUTE,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&mute));
    ESP_ERROR_CHECK(gpio_set_level(PIN_MUTE, 0));  // muted before anything else

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // Both directions on one controller, sharing BCLK and WS.
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, &s_rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = PIN_BCLK,
                .ws = PIN_WS,
                .dout = PIN_DOUT,
                .din = PIN_DIN,
                .invert_flags = {false, false, false},
            },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
}

static inline void amp_enable(bool on) { gpio_set_level(PIN_MUTE, on ? 1 : 0); }

// -------------------------------------------------------------------- wifi

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "wifi dropped, reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void) {
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, WIFI_PASSWORD, sizeof(wc.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Minimum modem sleep, which is also the default.
    //
    // WIFI_PS_NONE was tried here on the theory that beacon parking was
    // starving the uplink. It was not: the real cause of the dropped audio was
    // an unbounded send blocking the task loop. Keeping the radio awake did
    // measurably destabilise this board instead - five reconnects during boot
    // and the socket dying within half a second of streaming - most likely
    // because the amplifier shares the USB supply.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    ESP_LOGI(TAG, "connecting to \"%s\" (2.4 GHz only)", WIFI_SSID);
    xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "wifi up");
}

// --------------------------------------------------------------- websocket

// Control frames are matched by substring rather than parsed. The server sends
// exactly two shapes - {"type": "state", "value": ...} and {"type": "done"} -
// and the vocabularies do not overlap, so a full JSON parser would be weight
// for no benefit on a part with 400 KB of RAM.
static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    esp_websocket_event_data_t *e = (esp_websocket_event_data_t *)data;

    switch (id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "socket connected");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG,
                     "socket disconnected: sent %lu B, %lu send failures, "
                     "%lu dropped blocks, heap %lu",
                     (unsigned long)s_sent_bytes, (unsigned long)s_send_failures,
                     (unsigned long)s_dropped_blocks,
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
            s_state = ST_IDLE;
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "socket error: esp_tls=%d sock_errno=%d heap %lu",
                     e->error_handle.esp_tls_last_esp_err,
                     e->error_handle.esp_transport_sock_errno,
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
            break;
        case WEBSOCKET_EVENT_DATA:
            if (e->op_code == 0x02) {  // binary: reply audio
                // Block rather than drop. The server synthesises far faster
                // than real time - a 7-second reply arrives in about two - so
                // a non-blocking send silently threw most of it away and the
                // reply came out in fragments. Waiting here stalls this task,
                // which stops draining the socket, which is exactly the
                // backpressure TCP exists to provide.
                size_t queued = xStreamBufferSend(s_play_buf, e->data_ptr,
                                                  e->data_len, PLAY_SEND_TIMEOUT);
                if (queued != (size_t)e->data_len) {
                    s_play_dropped += (uint32_t)e->data_len - queued;
                }
            } else if (e->op_code == 0x01) {  // text: control
                if (memmem(e->data_ptr, e->data_len, "done", 4)) {
                    s_reply_finished = true;
                } else if (memmem(e->data_ptr, e->data_len, "speaking", 8)) {
                    s_state = ST_SPEAKING;
                } else if (memmem(e->data_ptr, e->data_len, "thinking", 8)) {
                    s_state = ST_THINKING;
                }
            }
            break;
        default:
            break;
    }
}

static void ws_start(void) {
    esp_websocket_client_config_t cfg = {
        .uri = SERVER_URI,
        .reconnect_timeout_ms = 2000,
        .network_timeout_ms = 5000,
        // Must exceed the chunk size below, or a send can wedge behind an
        // internal buffer that is smaller than what it is being handed.
        .buffer_size = 4096,
    };

    // Auth is a bearer token in the handshake; an open socket on a public URL
    // lets anyone drain the Groq free tier.
    static char headers[160];
    if (strlen(DEVICE_TOKEN) > 0) {
        snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", DEVICE_TOKEN);
        cfg.headers = headers;
    }

    s_ws = esp_websocket_client_init(&cfg);
    ESP_ERROR_CHECK(esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL));
    ESP_ERROR_CHECK(esp_websocket_client_start(s_ws));
}

// ------------------------------------------------------------------- tasks

static void audio_in_task(void *arg) {
    static int32_t raw[BLOCK_SAMPLES * 2];  // stereo frames
    static int16_t pcm[BLOCK_SAMPLES];

    while (true) {
        size_t got = 0;
        if (i2s_channel_read(s_rx, raw, sizeof(raw), &got, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        // Half duplex: the microphone is read continuously to keep the DMA
        // ring from overflowing, but only kept while the button is down.
        if (s_state != ST_LISTENING) continue;

        const size_t n = got / sizeof(int32_t) / 2;
        for (size_t i = 0; i < n; i++) {
            int32_t v = high_pass(raw[i * 2] >> 8) >> PCM_SHIFT;
            if (v > INT16_MAX) v = INT16_MAX;
            if (v < INT16_MIN) v = INT16_MIN;
            pcm[i] = (int16_t)v;
        }
        // Drop rather than block: a stalled uplink must not wedge the mic.
        // Dropping is counted, because silently losing audio looks exactly
        // like a bad microphone once it reaches the transcript.
        const size_t want = n * sizeof(int16_t);
        if (xStreamBufferSend(s_mic_buf, pcm, want, 0) != want) {
            s_dropped_blocks++;
        }
    }
}

static void audio_out_task(void *arg) {
    static uint8_t pcm[BLOCK_SAMPLES * 2];
    static int32_t frame[BLOCK_SAMPLES * 2];
    bool playing = false;

    while (true) {
        size_t got = xStreamBufferReceive(s_play_buf, pcm, sizeof(pcm), pdMS_TO_TICKS(20));

        if (got > 0) {
            if (!playing) {
                // Hold the amp shut until enough audio is queued to play
                // through the next gap in delivery.
                while (xStreamBufferBytesAvailable(s_play_buf) < PREBUFFER_BYTES &&
                       !s_reply_finished) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                amp_enable(true);
                playing = true;
                s_play_dropped = 0;
            }
            const size_t n = got / sizeof(int16_t);
            const int16_t *src = (const int16_t *)pcm;
            for (size_t i = 0; i < n; i++) {
                // int16 into the top half of a 32-bit slot; left channel only,
                // which is what the amplifier selects with SD driven high.
                frame[i * 2] = (int32_t)src[i] << 16;
                frame[i * 2 + 1] = 0;
            }
            size_t written = 0;
            i2s_channel_write(s_tx, frame, n * 2 * sizeof(int32_t), &written, portMAX_DELAY);
            continue;
        }

        // Nothing left and the server says the reply is over.
        if (playing && s_reply_finished) {
            vTaskDelay(pdMS_TO_TICKS(DRAIN_MS));  // do not clip the last word
            amp_enable(false);
            playing = false;
            s_reply_finished = false;
            s_state = ST_IDLE;
            ESP_LOGI(TAG, "idle (play dropped %lu B)", (unsigned long)s_play_dropped);
        } else if (s_reply_finished) {
            // The reply finished without producing a single byte of audio -
            // a TTS failure, most often. Nothing was playing, so the branch
            // above never runs, and without this the device would sit in
            // THINKING forever and silently ignore every button press.
            s_reply_finished = false;
            s_state = ST_IDLE;
            ESP_LOGW(TAG, "idle (reply produced no audio)");
        }
    }
}

static void net_task(void *arg) {
    static uint8_t chunk[1024];
    bool held = false;
    TickType_t changed = 0;

    TickType_t busy_since = 0;

    while (true) {
        const bool down = gpio_get_level(PIN_BUTTON) == 0;
        const TickType_t now = xTaskGetTickCount();

        // Last-resort unwedge. Any state but IDLE depends on the server
        // answering; if it never does, the button must still come back to life
        // rather than requiring a power cycle.
        if (s_state == ST_IDLE) {
            busy_since = now;
        } else if ((now - busy_since) > pdMS_TO_TICKS(STUCK_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "stuck in state %d for %d s, forcing idle", (int)s_state,
                     STUCK_TIMEOUT_MS / 1000);
            s_reply_finished = false;
            s_state = ST_IDLE;
            busy_since = now;
        }

        if (down != held && (now - changed) > pdMS_TO_TICKS(DEBOUNCE_MS)) {
            held = down;
            changed = now;

            if (held && s_state == ST_IDLE && esp_websocket_client_is_connected(s_ws)) {
                xStreamBufferReset(s_mic_buf);
                s_dropped_blocks = 0;
                s_sent_bytes = 0;
                s_send_failures = 0;
                s_state = ST_LISTENING;
                esp_websocket_client_send_text(s_ws, "{\"type\":\"start\"}", 16, SEND_TIMEOUT);
                ESP_LOGI(TAG, "listening");
            } else if (!held && s_state == ST_LISTENING) {
                // Flush whatever is still buffered before closing the utterance.
                size_t got;
                while ((got = xStreamBufferReceive(s_mic_buf, chunk, sizeof(chunk), 0)) > 0) {
                    esp_websocket_client_send_bin(s_ws, (char *)chunk, got, SEND_TIMEOUT);
                }
                esp_websocket_client_send_text(s_ws, "{\"type\":\"end\"}", 14, SEND_TIMEOUT);
                s_state = ST_THINKING;
                ESP_LOGI(TAG, "thinking: sent %lu B (%.1f s), %lu dropped, %lu failures, heap %lu",
                         (unsigned long)s_sent_bytes, s_sent_bytes / 32000.0f,
                         (unsigned long)s_dropped_blocks, (unsigned long)s_send_failures,
                         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
            }
        }

        // Stream while the button is held: this removes upload time from the
        // latency budget instead of sending the whole utterance on release.
        if (s_state == ST_LISTENING) {
            size_t got = xStreamBufferReceive(s_mic_buf, chunk, sizeof(chunk), pdMS_TO_TICKS(10));
            if (got > 0) {
                int rc = esp_websocket_client_send_bin(s_ws, (char *)chunk, got, SEND_TIMEOUT);
                if (rc < 0) {
                    s_send_failures++;  // uplink stalled; keep polling the button
                } else {
                    s_sent_bytes += (uint32_t)rc;
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());

    // Deliberately quiet on the hot path. The console is USB Serial/JTAG and
    // its writes are synchronous: with the transport at DEBUG level, every
    // audio frame emitted several log lines, and a host that was not draining
    // them fast enough blocked the task servicing the socket. The upload then
    // died a fraction of a second in, which is exactly the failure this
    // logging had been added to investigate.
    esp_log_level_set("websocket_client", ESP_LOG_WARN);
    esp_log_level_set("transport_ws", ESP_LOG_WARN);
    esp_log_level_set("transport", ESP_LOG_WARN);
    esp_log_level_set("transport_base", ESP_LOG_WARN);

    s_mic_buf = xStreamBufferCreate(MIC_BUFFER_BYTES, 1);
    s_play_buf = xStreamBufferCreate(PLAY_BUFFER_BYTES, 1);
    configASSERT(s_mic_buf && s_play_buf);

    audio_init();
    wifi_start();
    ws_start();

    xTaskCreate(audio_in_task, "audio_in", 4096, NULL, 5, NULL);
    xTaskCreate(audio_out_task, "audio_out", 4096, NULL, 5, NULL);
    xTaskCreate(net_task, "net", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "ready - hold the button on GPIO%d and speak", PIN_BUTTON);
}
