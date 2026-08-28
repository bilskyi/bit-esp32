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
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "config_store.h"
#include "face.h"
#include "provision.h"
#include "provision_logic.h"
#include "setup_screen.h"
#include "secrets.h"
#include "ssd1306.h"

#define PIN_BCLK GPIO_NUM_4
#define PIN_WS GPIO_NUM_5
#define PIN_DIN GPIO_NUM_6
#define PIN_DOUT GPIO_NUM_7
#define PIN_MUTE GPIO_NUM_10
#define PIN_BUTTON GPIO_NUM_3
// Onboard LED. Verified on this board: it lights when the pin is driven LOW.
//
// GPIO 8 is a strapping pin, which is why the spec warns against using it.
// That warning is about the level at reset, and idle here is LED off, which
// leaves the pin HIGH - the level a normal boot wants. Only pressing the
// button during a reset could interfere, and that is already true of BOOT.
#define PIN_LED GPIO_NUM_8
#define LED_ON 0
#define LED_OFF 1

// The OLED. Both pins were free; nothing else on this board wants them.
#define PIN_SDA GPIO_NUM_0
#define PIN_SCL GPIO_NUM_1
#define FACE_I2C_HZ 400000
#define FACE_FRAME_MS 40  // 25 fps

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
// One second of upload headroom, not half.
//
// Measured on a congested channel: single 1 KB sends stall for 1.6-2.3 s while
// RSSI stays at -55 dBm. At half a second of buffer such a stall discarded
// 1572 blocks and the server received a truncated utterance that Whisper read
// as "Что-то...". A second of slack turns most of those stalls into latency
// instead of lost speech.
// Sized in compressed bytes since ADPCM arrived: 16 KB is two seconds of
// speech, which is what a second of stall tolerance was meant to buy before
// compression made every byte worth four. Halving both buffers gives back
// 32 KB of heap, and TLS needs it - free heap fell from 69 KB to 30 KB the
// moment wss:// was switched on, and a handshake wants tens of KB transiently.
#define MIC_BUFFER_BYTES 16384
#define PLAY_BUFFER_BYTES 32768  // ~4 s of speech once compressed
// Do not open the amplifier until this much reply is in hand. The spec budgets
// 150 ms of playback buffer; starting earlier means the first word stutters
// while the network catches up.
// 150 ms was the spec's budget and is not enough here: sends stall for
// several hundred milliseconds at a time, so playback starts and immediately
// runs dry. Three quarters of a second of head start costs that much extra
// latency once, at the beginning, instead of stuttering throughout.
// Counted in compressed bytes: 0.4 s of speech is 3200 of them.
//
// Three quarters of a second was needed when the server trickled audio at a
// 1.2 s lead and the buffer never got ahead. Now that it fills to four
// seconds, a shorter head start is safe - and it comes straight off the wait
// before the first word.
#define PREBUFFER_CODED 3200

#define DEBOUNCE_MS 25
// Never block forever on a send. The button is polled in the same loop, so an
// unbounded wait means a release is never noticed and the device stays stuck
// in "listening" until it is reset - which is exactly what happened.
// Sends wait indefinitely on purpose.
//
// A bounded wait looks safer and is not: the timeout is handed straight to the
// transport's poll_write, and a poll that expires makes esp_transport_write()
// return 0, which the client treats as a fatal write error and tears the
// connection down. A full TCP send buffer for a few hundred milliseconds is
// ordinary on WiFi, so a 400 ms bound killed roughly every other upload.
//
// Blocking here is safe because the button is sampled by its own task, so a
// stalled send can no longer hide a release.
// Bounded, but generously.
//
// An expired poll makes the client tear the connection down, so this must sit
// well above a normal stall - at 400 ms it killed every other upload. But an
// unbounded wait is worse: a send that never returns holds the client lock
// forever, and the log fills with "Could not lock ws-client ... for CLOSE"
// while the device sits there unable to even hang up. Five seconds means a
// truly stuck link costs one reconnect instead of a wedge.
#define SEND_TIMEOUT pdMS_TO_TICKS(12000)
// Longest the device will wait on the server before re-arming the button.
#define STUCK_TIMEOUT_MS 20000

// How long the socket has to be down before the panel offers the way out.
// Long enough that an ordinary reconnect never shows it - the supervisor
// rebuilds the client at 15 s - short enough to be there when someone is
// standing in front of a device that is plainly not working.
#define OFFLINE_HINT_MS 8000
// How long the socket task may wait for room in the play buffer.
// Short on purpose. The server now paces the reply to roughly real time, so
// the buffer should almost never be full; blocking this task for seconds left
// the client stuck mid-frame and broke the read side instead.
#define PLAY_SEND_TIMEOUT pdMS_TO_TICKS(500)
#define DRAIN_MS 150  // let the DMA ring empty before cutting the amp

static const char *TAG = "voice";

static StreamBufferHandle_t s_mic_buf;
static StreamBufferHandle_t s_play_buf;
static EventGroupHandle_t s_wifi_events;
static esp_websocket_client_handle_t s_ws;

#define WIFI_CONNECTED_BIT BIT0

// Set while a provisioning trial is running. Outside a trial the handler
// behaves exactly as it always has, including reconnecting forever through a
// router reboot, which is what keeps the device alive.
static volatile bool s_trial_in_progress = false;
static volatile uint8_t s_trial_reason = 0;
#define WIFI_TRIAL_DONE_BIT BIT1
// Set by the hold-to-reset gesture. Only wifi_start()'s wait consumes it, and
// only to stop waiting - it is a request to go and provision, not a state.
#define WIFI_PROVISION_BIT BIT2

typedef enum { ST_IDLE, ST_LISTENING, ST_THINKING, ST_SPEAKING } state_t;
static volatile state_t s_state = ST_IDLE;
static volatile bool s_reply_finished = false;
static volatile uint32_t s_dropped_blocks = 0;
static volatile uint32_t s_sent_bytes = 0;
static volatile uint32_t s_send_failures = 0;
static volatile uint32_t s_play_dropped = 0;
static volatile uint32_t s_slowest_send = 0;
// Tick of the last sign of life from the server, for the stuck-state timer.
static volatile TickType_t s_last_activity = 0;
// Debounced button state, owned by button_task.
static volatile bool s_button_down = false;
// Set by net_task when the user presses during a reply; audio_out acts on it.
static volatile bool s_abort_playback = false;
// Set when a reply is cancelled, cleared by the "done" that closes it.
//
// Resetting the play buffer only discards what has already arrived. The server
// runs up to playback_lead_s ahead of real time - four seconds - so the rest of
// a cancelled reply is still in flight, and without this it lands in the buffer
// a moment later, the amp comes back on, and the device carries on talking
// after being told to stop. The server sends "done" at the end of every reply
// including a cancelled one, so it is exactly the right boundary.
static volatile bool s_discard_audio = false;
static volatile uint32_t s_discarded_bytes = 0;
// When the socket last went away, for the reconnect supervisor.
static volatile TickType_t s_offline_since = 0;

// -- the face ---------------------------------------------------------------
//
// face_task owns the face_t and the panel and touches nothing else. Everything
// it needs arrives through the volatile scalars below, written by whichever
// task happens to know: the socket task knows the emotion, net_task knows an
// interruption happened, the audio tasks know how loud things are. Nobody else
// ever reaches into the animation, so there is no lock and nothing to wedge
// the audio path.
static ssd1306_t s_panel;
static face_t s_face;
static bool s_have_panel = false;

static volatile uint8_t s_face_emotion = FACE_EMO_NEUTRAL;
// Bumped on every emotion frame, so the same emotion twice in a row still
// counts as the conversation being alive.
static volatile uint32_t s_face_emotion_seq = 0;
static volatile bool s_face_startle = false;
// How far through the hold-to-reset gesture the button is, 0-100. Owned by
// net_task, read by face_task, the same shape as the two above. Zero means
// not counting, and face.c treats zero as leaving no trace at all - releasing
// the button has to cost nothing.
static volatile uint8_t s_face_reset_pct = 0;
// Mean absolute sample of the last audio block, either direction.
static volatile uint16_t s_audio_level = 0;
// How far boot has got. Set by the three places that already log these very
// milestones; the face opens its eyes exactly that far.
static volatile uint8_t s_boot_stage = FACE_BOOT_PANEL;

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// Cheap loudness for the face. Only the shape matters - face.c normalises it
// against its own decaying peak - so the mean absolute sample does as well as
// an RMS and costs no multiply.
static uint16_t block_level(const int16_t *pcm, size_t n) {
    if (n == 0) return 0;
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t v = pcm[i];
        if (v < 0) v = -v;  // via int32: negating INT16_MIN as int16 overflows
        sum += (uint32_t)v;
    }
    return (uint16_t)(sum / n);
}

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

// ------------------------------------------------------------------- adpcm

// IMA/DVI ADPCM: four bits per sample instead of sixteen.
//
// The uplink needs 32 KB/s for raw 16 kHz PCM and this link does not provide
// it - sends stall for one to two seconds while RSSI sits at a healthy -55 dBm,
// and a truncated utterance reaches Whisper as nonsense. At 8 KB/s the same
// link carries a whole question.
//
// This is not the Opus the spec ruled out: no tables beyond the two below, no
// allocation, a few dozen arithmetic operations per sample. The decoder on the
// server is checked against the reference implementation.
static const int16_t ADPCM_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
    230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
    963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749,
    3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};

static const int8_t ADPCM_INDEX[16] = {-1, -1, -1, -1, 2, 4, 6, 8,
                                       -1, -1, -1, -1, 2, 4, 6, 8};

static int32_t s_adpcm_pred = 0;
static int32_t s_adpcm_index = 0;

static void adpcm_reset(void) {
    s_adpcm_pred = 0;
    s_adpcm_index = 0;
}

static inline uint8_t adpcm_encode_sample(int16_t sample) {
    int32_t step = ADPCM_STEP[s_adpcm_index];
    int32_t diff = (int32_t)sample - s_adpcm_pred;

    uint8_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }

    // Three magnitude bits, each worth half of the one before, with the
    // reconstruction accumulated alongside so both ends stay in step.
    int32_t vpdiff = step >> 3;
    if (diff >= step) {
        code |= 4;
        diff -= step;
        vpdiff += step;
    }
    step >>= 1;
    if (diff >= step) {
        code |= 2;
        diff -= step;
        vpdiff += step;
    }
    step >>= 1;
    if (diff >= step) {
        code |= 1;
        vpdiff += step;
    }

    s_adpcm_pred += (code & 8) ? -vpdiff : vpdiff;
    if (s_adpcm_pred > 32767) s_adpcm_pred = 32767;
    if (s_adpcm_pred < -32768) s_adpcm_pred = -32768;

    s_adpcm_index += ADPCM_INDEX[code];
    if (s_adpcm_index < 0) s_adpcm_index = 0;
    if (s_adpcm_index > 88) s_adpcm_index = 88;

    return code & 0x0F;
}

// Packs into half as many bytes, earlier sample in the high nibble. n must be
// even, which BLOCK_SAMPLES is, so no nibble is ever left pending between
// blocks.
static size_t adpcm_encode_block(const int16_t *in, size_t n, uint8_t *out) {
    size_t written = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        const uint8_t hi = adpcm_encode_sample(in[i]);
        const uint8_t lo = adpcm_encode_sample(in[i + 1]);
        out[written++] = (uint8_t)((hi << 4) | lo);
    }
    return written;
}

// Decoder state for the reply stream. Separate from the encoder's: the two
// directions are independent streams and must not share a predictor.
static int32_t s_adpcm_rx_pred = 0;
static int32_t s_adpcm_rx_index = 0;

static void adpcm_rx_reset(void) {
    s_adpcm_rx_pred = 0;
    s_adpcm_rx_index = 0;
}

static inline int16_t adpcm_decode_code(uint8_t code) {
    const int32_t step = ADPCM_STEP[s_adpcm_rx_index];

    int32_t vpdiff = step >> 3;
    if (code & 4) vpdiff += step;
    if (code & 2) vpdiff += step >> 1;
    if (code & 1) vpdiff += step >> 2;

    s_adpcm_rx_pred += (code & 8) ? -vpdiff : vpdiff;
    if (s_adpcm_rx_pred > 32767) s_adpcm_rx_pred = 32767;
    if (s_adpcm_rx_pred < -32768) s_adpcm_rx_pred = -32768;

    s_adpcm_rx_index += ADPCM_INDEX[code];
    if (s_adpcm_rx_index < 0) s_adpcm_rx_index = 0;
    if (s_adpcm_rx_index > 88) s_adpcm_rx_index = 88;

    return (int16_t)s_adpcm_rx_pred;
}

// Expands in place-ish: n coded bytes become 2n samples. Earlier sample in the
// high nibble, matching the server.
static size_t adpcm_decode_block(const uint8_t *in, size_t n, int16_t *out) {
    size_t written = 0;
    for (size_t i = 0; i < n; i++) {
        out[written++] = adpcm_decode_code((in[i] >> 4) & 0x0F);
        out[written++] = adpcm_decode_code(in[i] & 0x0F);
    }
    return written;
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
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
        if (s_trial_in_progress) {
            // A trial asks a question, so a disconnect is the answer, not a fault
            // to recover from. The reason code is what separates "wrong password"
            // from "that network is not here", and telling them apart is most of
            // the value of trialling before saving.
            s_trial_reason = d->reason;
            xEventGroupSetBits(s_wifi_events, WIFI_TRIAL_DONE_BIT);
            return;
        }
        ESP_LOGW(TAG, "wifi dropped, reconnecting (reason %d)", (int)d->reason);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

// The rest of this section is provision.c's window into wifi_event() above -
// it owns s_wifi_events, s_trial_in_progress and s_trial_reason, none of
// which are safe to touch from another translation unit directly, so
// provision.h declares these two and this file defines them.

// Turns trial mode on or off around a credential trial run from POST /save.
// While on, wifi_event() ends a disconnect at the reason code instead of
// retrying it, per s_trial_in_progress above. Switching on also clears
// WIFI_CONNECTED_BIT and WIFI_TRIAL_DONE_BIT, so a bit left over from normal
// operation before provisioning started, or from an earlier trial this same
// session, cannot make provision_wifi_trial_wait() below return a stale
// answer before this attempt has actually run.
void provision_set_trial_mode(bool on) {
    if (s_wifi_events == NULL) {
        // wifi_start() is what creates s_wifi_events, and Task 7 - wiring
        // provisioning into the boot sequence - is what is supposed to
        // guarantee it runs before provision_start() ever can. This guard
        // does not change that ordering; it exists for the day something
        // violates it anyway. Every FreeRTOS event-group call below takes a
        // handle with no NULL check of its own, so skipping straight to one
        // is a fault with a symptom that points nowhere near this line. A
        // log line that names the actual missing dependency is a strictly
        // better failure than that.
        ESP_LOGE(TAG, "provision_set_trial_mode: s_wifi_events not ready (wifi_start() has not run)");
        return;
    }
    if (on) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_TRIAL_DONE_BIT);
    }
    s_trial_in_progress = on;
}

// Blocks for up to PROV_TRIAL_MS waiting for the esp_wifi_connect() a trial
// just started to resolve one way or the other. Call only while trial mode
// is on, after esp_wifi_connect() has been issued.
provision_trial_outcome_t provision_wifi_trial_wait(uint8_t *out_reason) {
    if (s_wifi_events == NULL) {
        // Same missing-dependency guard as provision_set_trial_mode() above,
        // for the same reason. PROV_TRIAL_TIMED_OUT is the honest answer for
        // "nothing could even be asked" - it is the same outcome
        // save_post_handler() already falls back to when
        // esp_wifi_set_config()/esp_wifi_connect() fails synchronously and
        // there is nothing to wait on, so this introduces no new case for
        // callers to handle.
        ESP_LOGE(TAG, "provision_wifi_trial_wait: s_wifi_events not ready (wifi_start() has not run)");
        return PROV_TRIAL_TIMED_OUT;
    }
    const EventBits_t bits =
        xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_TRIAL_DONE_BIT,
                             pdFALSE, pdFALSE, pdMS_TO_TICKS(PROV_TRIAL_MS));
    if (bits & WIFI_CONNECTED_BIT) return PROV_TRIAL_CONNECTED;
    if (bits & WIFI_TRIAL_DONE_BIT) {
        *out_reason = s_trial_reason;
        return PROV_TRIAL_DISCONNECTED;
    }
    return PROV_TRIAL_TIMED_OUT;
}

// Bounded wait for provision_wifi_trial_cancel() below. esp_http_server
// runs every connection on one task (see PROV_BODY_RECV_TIMEOUT_MS in
// provision.c), so this can never become a new way to hang it - the whole
// point is to close a race, not open a hang in its place. What it is
// actually waiting on is WiFi-driver teardown plus one hop through the
// default event-loop queue, which is single-digit milliseconds in
// practice; 300 ms is generously above that while still costing nothing
// worth noticing next to the up-to-PROV_TRIAL_MS (20 s) trial this follows.
#define PROV_TRIAL_CANCEL_WAIT_MS 300

// Cancels the connection attempt a trial started and waits for
// confirmation that wifi_event() saw the resulting disconnect on the trial
// branch, rather than assuming it did. Textual order relative to
// provision_set_trial_mode(false) is not synchronisation:
// esp_wifi_disconnect() only *requests* the disconnect, and the
// WIFI_EVENT_STA_DISCONNECTED it produces has to cross WiFi-driver
// teardown and then dispatch on this event-loop task before wifi_event()
// ever runs, while clearing s_trial_in_progress is a same-thread write
// that finishes essentially instantly - so the flag is very likely to
// clear first, routing the cancellation's own event to the branch that
// reconnects unconditionally, forever, with the candidate the caller just
// decided had failed.
//
// WIFI_TRIAL_DONE_BIT is cleared before esp_wifi_disconnect() is even
// called, not just before the wait: the trial outcome the caller just read
// may have left that bit set (provision_wifi_trial_wait() does not clear
// on exit), and clearing it only after issuing the disconnect could just
// as easily wipe out the real confirmation as the stale one. Call only
// while trial mode is still on, after a trial outcome other than
// PROV_TRIAL_CONNECTED.
void provision_wifi_trial_cancel(void) {
    if (s_wifi_events == NULL) {
        // Same missing-dependency guard as provision_set_trial_mode() and
        // provision_wifi_trial_wait() above, for the same reason.
        ESP_LOGE(TAG, "provision_wifi_trial_cancel: s_wifi_events not ready (wifi_start() has not run)");
        return;
    }

    xEventGroupClearBits(s_wifi_events, WIFI_TRIAL_DONE_BIT);

    const esp_err_t derr = esp_wifi_disconnect();
    if (derr != ESP_OK) {
        // Nothing was actually requested - most likely the station was
        // already idle, which is exactly what a prior PROV_TRIAL_DISCONNECTED
        // or a synchronous esp_wifi_connect() failure leaves behind - so no
        // event will ever set the bit. Waiting would only spend
        // PROV_TRIAL_CANCEL_WAIT_MS to learn what is already known.
        ESP_LOGW(TAG, "esp_wifi_disconnect (trial cleanup): %s", esp_err_to_name(derr));
        return;
    }

    const EventBits_t bits =
        xEventGroupWaitBits(s_wifi_events, WIFI_TRIAL_DONE_BIT, pdFALSE, pdFALSE,
                             pdMS_TO_TICKS(PROV_TRIAL_CANCEL_WAIT_MS));
    if (!(bits & WIFI_TRIAL_DONE_BIT)) {
        // The wait gave up, not the disconnect - that request is still in
        // flight and may yet resolve after this function returns. The
        // caller proceeds regardless: this has narrowed the window for the
        // stray event to land on the wrong branch, not closed it, which is
        // the honest limit of what a bounded wait on a single-tasked HTTP
        // server can do.
        ESP_LOGW(TAG, "trial cancel: no confirmation within %d ms; proceeding anyway",
                 PROV_TRIAL_CANCEL_WAIT_MS);
    }
}

// Takes the credentials rather than reading the macros, so config_store's
// values - which is to say whatever a phone last saved - are what the radio
// actually joins. Reading WIFI_SSID here instead would leave provisioning
// writing a value nothing ever reads.
// Everything that may happen exactly once, split out from connecting.
//
// It is separate because provisioning needs the stack up before it runs -
// esp_wifi_set_storage(), esp_wifi_set_mode() and the rest all return
// ESP_ERR_WIFI_NOT_INIT otherwise - and provisioning happens before there is
// any network to join. The first board test of this feature failed exactly
// here: provision_start() ran ahead of esp_wifi_init() and could not start,
// so the device fell through to waiting forever on an empty SSID.
//
// It also creates s_wifi_events, which provision.c's trial functions wait on.
static void wifi_init_stack(void) {
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
}

static void wifi_connect(const char *ssid, const char *pass) {
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);

    // Wake for every beacon rather than every third.
    //
    // Measured: pings to this device lose nothing but arrive anywhere between
    // 3 ms and 2075 ms, and the boot log reports a listen interval of three
    // beacons - 307 ms. That queuing is what the uplink experiences as stalls.
    // Turning power save off entirely was tried twice and is worse: a single
    // 1 KB send took 32 seconds and the radio then failed to associate at all.
    // Waking three times as often is the middle ground.
    wc.sta.listen_interval = 1;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Minimum modem sleep. Do not "fix" the stalls by turning this off.
    //
    // WIFI_PS_NONE has now been tried twice on the theory that beacon parking
    // causes the send stalls. The second attempt, with the keepalive bug
    // already fixed, was measured: one 1 KB send took 32 seconds, 986 audio
    // blocks were dropped, and the device then failed to open a connection at
    // all, retrying every seven seconds with ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT
    // until it was reflashed. Keeping the radio awake does not make this link
    // faster, it makes it unusable - most likely supply, since the amplifier
    // shares USB power.
    //
    // The stalls are real but must be absorbed, not eliminated.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    ESP_LOGI(TAG, "connecting to \"%s\" (2.4 GHz only)", ssid);

    // Still no timeout: the device waits for its network exactly as long as it
    // always has, because a router that is rebooting is worth waiting out and
    // falling back to an access point would turn a two-minute outage into a
    // device that has stopped being a voice companion.
    //
    // What changed is that it stops being unreachable while it waits. The
    // hold-to-reset gesture sets WIFI_PROVISION_BIT, and without it in this
    // wait the task holding the boot sequence never returns - so the button
    // would be undetectable in exactly the situation that needs it.
    const EventBits_t up = xEventGroupWaitBits(
        s_wifi_events, WIFI_CONNECTED_BIT | WIFI_PROVISION_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    if (up & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "wifi up");
        s_boot_stage = FACE_BOOT_WIFI;  // eyes half open: an IP, but no server yet
    } else {
        ESP_LOGW(TAG, "giving up on \"%s\": provisioning was asked for", ssid);
    }
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
            s_offline_since = 0;
            s_boot_stage = FACE_BOOT_LINK;  // the face can finish waking up
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
            // The client fills data_ptr with a readable description of what
            // went wrong. The numeric fields alone were not enough to identify
            // this failure across several sessions.
            ESP_LOGE(TAG, "socket error type=%d esp_err=%d sock_errno=%d heap %lu: %.*s",
                     (int)e->error_handle.error_type,
                     e->error_handle.esp_tls_last_esp_err,
                     e->error_handle.esp_transport_sock_errno,
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                     e->data_len, e->data_ptr ? e->data_ptr : "");
            break;
        case WEBSOCKET_EVENT_DATA:
            s_last_activity = xTaskGetTickCount();
            if (e->op_code == 0x02 && s_discard_audio) {
                // The tail of a reply the user already interrupted. Dropping it
                // here rather than in audio_out_task matters: this send blocks
                // when the buffer is full, so queueing audio nobody will hear
                // also stalls the task that drains the socket.
                s_discarded_bytes += (uint32_t)e->data_len;
            } else if (e->op_code == 0x02) {  // binary: reply audio
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
                // Emotion first, and only inside a frame that says it is one.
                // Scanning every text frame for every emotion name would be
                // asking for a collision the day a new state is added.
                if (memmem(e->data_ptr, e->data_len, "emotion", 7)) {
                    const face_emotion_t em =
                        face_emotion_scan(e->data_ptr, e->data_len);
                    if (em != FACE_EMO_COUNT) {
                        s_face_emotion = (uint8_t)em;
                        s_face_emotion_seq++;
                    } else {
                        // The server promised one of nine names. Anything else
                        // means the two halves have drifted apart, and a face
                        // that silently keeps its old expression hides that.
                        ESP_LOGW(TAG, "unknown emotion: %.*s", e->data_len,
                                 e->data_ptr ? e->data_ptr : "");
                    }
                } else if (memmem(e->data_ptr, e->data_len, "done", 4)) {
                    s_reply_finished = true;
                    if (s_discard_audio) {
                        // Belt and braces. "speaking" above is the boundary
                        // that matters; this only catches the case where the
                        // cancelled reply's done arrives and no new question
                        // follows, so the flag is not left raised into the
                        // next turn.
                        s_discard_audio = false;
                        ESP_LOGI(TAG, "dropped %lu B, no new reply followed",
                                 (unsigned long)s_discarded_bytes);
                        s_discarded_bytes = 0;
                    }
                } else if (memmem(e->data_ptr, e->data_len, "speaking", 8)) {
                    s_state = ST_SPEAKING;
                    // The real end of the discard window, and the reason it
                    // cannot be "done".
                    //
                    // Measured: the done closing a cancelled reply arrived
                    // 0.3 s after the interrupt twice and 7 s twice, and in
                    // the slow cases the user had already asked again - so
                    // the answer to the new question was thrown away as
                    // though it were the tail of the old one. The server
                    // sends this immediately before the first audio of every
                    // reply, and the cancelled reply's tail cannot arrive
                    // after it, because the server cancelled that task before
                    // starting this turn.
                    if (s_discard_audio) {
                        s_discard_audio = false;
                        ESP_LOGI(TAG, "dropped %lu B of an interrupted reply",
                                 (unsigned long)s_discarded_bytes);
                        s_discarded_bytes = 0;
                    }
                } else if (memmem(e->data_ptr, e->data_len, "thinking", 8)) {
                    s_state = ST_THINKING;
                }
            }
            break;
        default:
            break;
    }
}

// Takes the URI for the same reason wifi_start() takes the credentials: it is
// settable from the phone, and reading the macro here would make that setting
// a value nothing reads.
static void ws_start(const char *uri) {
    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .reconnect_timeout_ms = 2000,
        .network_timeout_ms = 5000,
        // Must exceed the chunk size below, or a send can wedge behind an
        // internal buffer that is smaller than what it is being handed.
        .buffer_size = 4096,
        // Keepalive must not police a peer that is merely busy.
        //
        // Uploading holds the client lock in bursts, and a stalled send once
        // blocked a PONG for long enough that the client declared the server
        // gone and dropped a working connection. A dead link is still caught:
        // reads fail and the transport reports it. This only stops a missed
        // PONG from being a death sentence.
        .ping_interval_sec = 20,
        .pingpong_timeout_sec = 60,
        .disable_pingpong_discon = true,
    };

    // Auth is a bearer token in the handshake; an open socket on a public URL
    // lets anyone drain the Groq free tier.
    static char headers[160];
    if (strlen(DEVICE_TOKEN) > 0) {
        snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", DEVICE_TOKEN);
        cfg.headers = headers;
    }

    // wss:// verifies the server against the root certificates bundled into
    // the image. Plain ws:// on the bench needs none of this, so it is
    // attached only when the URI actually asks for TLS.
    if (strncmp(SERVER_URI, "wss://", 6) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        ESP_LOGI(TAG, "TLS enabled, verifying against the bundled roots");
    }

    s_ws = esp_websocket_client_init(&cfg);
    ESP_ERROR_CHECK(esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL));
    ESP_ERROR_CHECK(esp_websocket_client_start(s_ws));
}

// --------------------------------------------------------------------- led

// One tick is 100 ms; every pattern below is expressed in ticks.
static void led_task(void *arg) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_LED,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(PIN_LED, LED_OFF);

    uint32_t tick = 0;
    while (true) {
        bool lit;

        if (s_ws == NULL || !esp_websocket_client_is_connected(s_ws)) {
            // Two quick blinks every two seconds: not connected, a press will
            // go nowhere. Worth showing, because the keepalive can take most
            // of a minute to notice a link that died quietly.
            const uint32_t phase = tick % 20;
            lit = (phase == 0 || phase == 2);
        } else {
            switch (s_state) {
                case ST_LISTENING:
                    lit = true;  // steady: talk now
                    break;
                case ST_THINKING:
                    lit = (tick % 2) == 0;  // 5 Hz: working
                    break;
                case ST_SPEAKING:
                    lit = (tick % 10) < 5;  // 1 Hz: talking, press to interrupt
                    break;
                case ST_IDLE:
                default:
                    lit = false;  // dark: ready
                    break;
            }
        }

        gpio_set_level(PIN_LED, lit ? LED_ON : LED_OFF);
        vTaskDelay(pdMS_TO_TICKS(100));
        tick++;
    }
}

// -------------------------------------------------------------------- face

// The same shape as led_task: a fixed tick that reads s_state and draws. It
// runs below the audio tasks and the button, holds no lock, and the I2C write
// blocks on a semaphore rather than spinning, so a slow panel costs frames
// here and nothing anywhere else.
static void face_task(void *arg) {
    face_init(&s_face, now_ms());

    face_state_t shown = FACE_ST_IDLE;
    uint32_t emotion_seq = 0;
    bool button = false;

    while (true) {
        const uint32_t t = now_ms();

        // The power-on sequence owns the panel until it hands over, and it
        // needs to know how far the device has got. Idempotent, so there is
        // nothing to latch here.
        face_boot_stage(&s_face, (face_boot_t)s_boot_stage, t);

        // A press that goes nowhere still deserves an answer, so the socket
        // being down is a state of its own rather than an absence of one.
        const bool linked = s_ws != NULL && esp_websocket_client_is_connected(s_ws);
        face_state_t want = FACE_ST_OFFLINE;
        if (linked) {
            switch (s_state) {
                case ST_LISTENING: want = FACE_ST_LISTENING; break;
                case ST_THINKING:  want = FACE_ST_THINKING;  break;
                case ST_SPEAKING:  want = FACE_ST_SPEAKING;  break;
                case ST_IDLE:
                default:           want = FACE_ST_IDLE;      break;
            }
        }
        if (want != shown) {
            face_set_state(&s_face, want, t);
            shown = want;
        }

        const uint32_t seq = s_face_emotion_seq;
        if (seq != emotion_seq) {
            emotion_seq = seq;
            face_set_emotion(&s_face, (face_emotion_t)s_face_emotion, t);
        }

        const bool down = s_button_down;
        if (down != button) {
            face_set_button(&s_face, down, t);
            button = down;
        }

        if (s_face_startle) {
            s_face_startle = false;
            face_startle(&s_face, t);
        }

        // Only while there is audio. Outside those states face.c lets the
        // energy decay on its own, which is what stops the eyes freezing
        // mid-syllable when a reply ends.
        if (want == FACE_ST_LISTENING || want == FACE_ST_SPEAKING) {
            face_feed_energy(&s_face, s_audio_level);
        }

        // One task draws, and it chooses which of the two things to draw.
        //
        // setup_screen.c renders into a buffer it is handed and never touches
        // the panel, so this is the only writer either way and no lock is
        // needed. The setup screen gets a buffer of its own rather than
        // borrowing face_t's private one: reaching into another module's
        // state is exactly what its no-dependency rule exists to prevent.
        // 1 KB of BSS, not heap.
        static uint8_t s_setup_fb[FACE_FB_BYTES];
        const uint8_t *fb;
        if (provision_is_active()) {
            setup_screen_t s;
            provision_screen(&s);
            setup_screen_render(s_setup_fb, &s);
            fb = s_setup_fb;
        } else {
            face_set_reset_progress(&s_face, s_face_reset_pct, t);
            face_tick(&s_face, t);
            fb = face_framebuffer(&s_face);

            // A gesture nobody can discover is knowledge that lives in one
            // person's head. Say it on the panel in the one situation where
            // it is needed - the device visibly stuck with no server - and
            // nowhere else, so it does not become furniture.
            //
            // The face's own buffer is not written to: it is copied and the
            // line goes on the copy. face.c owns f->fb and this is the whole
            // reason setup_screen.c renders into a buffer it is handed.
            const TickType_t off = s_offline_since;
            if (off != 0 &&
                (uint32_t)(xTaskGetTickCount() - off) * portTICK_PERIOD_MS > OFFLINE_HINT_MS) {
                memcpy(s_setup_fb, fb, FACE_FB_BYTES);
                ss_draw_text(s_setup_fb, 0, FACE_H - SS_GLYPH_H, "5 presses = setup");
                fb = s_setup_fb;
            }
        }

        // A panel that stops acknowledging - a wire off, a brownout - must be
        // visible in the log without filling it. Say so on the way down and on
        // the way back, and nothing in between.
        static bool panel_ok = true;
        const esp_err_t ferr = ssd1306_flush(&s_panel, fb);
        if ((ferr == ESP_OK) != panel_ok) {
            panel_ok = (ferr == ESP_OK);
            if (panel_ok) {
                ESP_LOGI(TAG, "panel back");
            } else {
                ESP_LOGW(TAG, "panel stopped answering: %s", esp_err_to_name(ferr));
            }
        }

        // Pace against the clock, so a slow flush eats the idle time instead
        // of stretching the frame.
        const uint32_t spent = now_ms() - t;
        vTaskDelay(pdMS_TO_TICKS(spent >= FACE_FRAME_MS ? 1 : FACE_FRAME_MS - spent));
    }
}

// Drags the socket back when the client's own retry gives up.
//
// Observed: after a server restart, or after a link stall long enough to kill
// the connection, the client would sit reporting "not connected" indefinitely
// while the device looked broken from outside. Its internal retry is not
// always enough, so this tears the client down and builds it again - and if
// even that fails for long enough, reboots, because a device that recovers by
// itself in ninety seconds beats one that waits for a human.
#define RECONNECT_AFTER_MS 15000
#define REBOOT_AFTER_MS 90000

static void link_task(void *arg) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (s_ws == NULL) continue;

        if (esp_websocket_client_is_connected(s_ws)) {
            s_offline_since = 0;
            continue;
        }

        const TickType_t now = xTaskGetTickCount();
        if (s_offline_since == 0) {
            s_offline_since = now;
            continue;
        }

        const uint32_t down_ms = (uint32_t)(now - s_offline_since) * portTICK_PERIOD_MS;

        if (down_ms > REBOOT_AFTER_MS) {
            ESP_LOGE(TAG, "offline for %lu s, restarting", (unsigned long)(down_ms / 1000));
            esp_restart();
        }

        if (down_ms > RECONNECT_AFTER_MS) {
            ESP_LOGW(TAG, "offline for %lu s, rebuilding the client",
                     (unsigned long)(down_ms / 1000));
            esp_websocket_client_stop(s_ws);
            esp_websocket_client_start(s_ws);
            s_offline_since = now;  // give the fresh client its own window
        }
    }
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
        //
        // Both conditions matter. s_state is owned by net_task, which can sit
        // inside a send for many seconds on a congested link - during which it
        // cannot notice that the button came up. Checking the debounced button
        // directly stops recording the instant it is released, so a three
        // second question stays three seconds instead of growing to twenty-two
        // and burying the uplink in audio nobody asked for.
        if (s_state != ST_LISTENING || !s_button_down) continue;

        const size_t n = got / sizeof(int32_t) / 2;
        for (size_t i = 0; i < n; i++) {
            int32_t v = high_pass(raw[i * 2] >> 8) >> PCM_SHIFT;
            if (v > INT16_MAX) v = INT16_MAX;
            if (v < INT16_MIN) v = INT16_MIN;
            pcm[i] = (int16_t)v;
        }

        // Hand the face your voice, so the eyes widen when you get louder.
        s_audio_level = block_level(pcm, n);

        // Compress before buffering, so the buffer holds four times as much
        // speech for the same RAM and the uplink carries a quarter as much.
        static uint8_t coded[BLOCK_SAMPLES / 2];
        const size_t coded_len = adpcm_encode_block(pcm, n, coded);

        // Drop rather than block: a stalled uplink must not wedge the mic.
        // Dropping is counted, because silently losing audio looks exactly
        // like a bad microphone once it reaches the transcript.
        if (xStreamBufferSend(s_mic_buf, coded, coded_len, 0) != coded_len) {
            s_dropped_blocks++;
        }
    }
}

static void audio_out_task(void *arg) {
    // The play buffer now holds compressed audio, so the same RAM rides out
    // four times as long a gap: 48 KB is about six seconds of speech.
    static uint8_t coded[BLOCK_SAMPLES / 2];
    static int16_t pcm[BLOCK_SAMPLES];
    static int32_t frame[BLOCK_SAMPLES * 2];
    bool playing = false;
    TickType_t play_started = 0;
    uint32_t played_bytes = 0;
    uint32_t starved = 0;  // times the buffer ran dry mid-reply

    while (true) {
        if (s_abort_playback) {
            // Mute before discarding, so nothing half-written escapes.
            amp_enable(false);
            xStreamBufferReset(s_play_buf);
            // The stream buffer is not the only place audio waits. Whatever
            // has already been handed to i2s_channel_write sits in the DMA
            // ring, and muting the amp only hides it - it plays as a burst of
            // the abandoned reply the moment the amp comes back on for the
            // next one. Cycling the channel is what actually empties it.
            i2s_channel_disable(s_tx);
            i2s_channel_enable(s_tx);
            s_audio_level = 0;
            playing = false;
            s_reply_finished = false;
            s_abort_playback = false;
            ESP_LOGI(TAG, "playback aborted");
            continue;
        }

        size_t got = xStreamBufferReceive(s_play_buf, coded, sizeof(coded), pdMS_TO_TICKS(20));

        if (got > 0) {
            if (!playing) {
                play_started = xTaskGetTickCount();
                played_bytes = 0;
                starved = 0;
                adpcm_rx_reset();
                // Hold the amp shut until enough audio is queued to play
                // through the next gap in delivery.
                //
                // The abort has to be visible from in here. Without that check
                // an interrupt arriving while this waits was ignored until the
                // wait ended, and then amp_enable(true) below switched the
                // speaker back on - the abort had muted it a moment earlier.
                // That is the "does not go silent immediately": not latency,
                // but the playback task undoing the mute.
                while (xStreamBufferBytesAvailable(s_play_buf) < PREBUFFER_CODED &&
                       !s_reply_finished && !s_abort_playback) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                if (s_abort_playback) continue;  // handled at the top, amp stays shut
                amp_enable(true);
                playing = true;
                s_play_dropped = 0;
                ESP_LOGI(TAG, "playing");
            }
            const size_t n = adpcm_decode_block(coded, got, pcm);

            // This is why the face needed no mouth: the reply's own loudness
            // squashes the eyes on every syllable, from the samples that are
            // about to be played rather than from a guess about timing.
            s_audio_level = block_level(pcm, n);

            for (size_t i = 0; i < n; i++) {
                // int16 into the top half of a 32-bit slot; left channel only,
                // which is what the amplifier selects with SD driven high.
                frame[i * 2] = (int32_t)pcm[i] << 16;
                frame[i * 2 + 1] = 0;
            }
            size_t written = 0;
            i2s_channel_write(s_tx, frame, n * 2 * sizeof(int32_t), &written, portMAX_DELAY);
            played_bytes += (uint32_t)(n * sizeof(int16_t));
            continue;
        }

        if (playing && !s_reply_finished) {
            // Buffer empty while the reply is still coming.
            //
            // The I2S peripheral does not stop when it runs out of data - the
            // DMA ring keeps cycling whatever was in it, so an underrun is
            // heard as the last fragment repeating, fast, for as long as the
            // gap lasts. That is the "ца-ца-ца" that ran for twenty seconds.
            // Feeding it silence turns a stuck syllable into an honest pause.
            starved++;
            memset(frame, 0, sizeof(frame));
            size_t written = 0;
            i2s_channel_write(s_tx, frame, sizeof(frame), &written, pdMS_TO_TICKS(100));
        }

        // Nothing left and the server says the reply is over.
        if (playing && s_reply_finished) {
            vTaskDelay(pdMS_TO_TICKS(DRAIN_MS));  // do not clip the last word
            amp_enable(false);
            s_audio_level = 0;
            playing = false;
            s_reply_finished = false;
            s_state = ST_IDLE;
            {
                const uint32_t ms = (uint32_t)(xTaskGetTickCount() - play_started) * portTICK_PERIOD_MS;
                ESP_LOGI(TAG,
                         "idle: played %lu B (%.1f s of audio) in %.1f s, "
                         "starved %lu times, dropped %lu B",
                         (unsigned long)played_bytes, played_bytes / 32000.0f,
                         ms / 1000.0f, (unsigned long)starved,
                         (unsigned long)s_play_dropped);
            }
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

// Samples the button and nothing else, so its timing cannot be affected by a
// send that is waiting on the network.
static void button_task(void *arg) {
    bool stable = false;
    TickType_t changed = 0;

    while (true) {
        const bool down = gpio_get_level(PIN_BUTTON) == 0;
        const TickType_t now = xTaskGetTickCount();
        if (down != stable && (now - changed) > pdMS_TO_TICKS(DEBOUNCE_MS)) {
            stable = down;
            changed = now;
            s_button_down = down;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// Hold the button, stay quiet, and the device goes off to be reconfigured.
//
// The silence is what makes this safe. A held button is also how you ask a
// long question, and s_audio_level already knows whether anyone is talking -
// block_level() computes it per block and the eyes are already driven by it -
// so gating on quiet means an ordinary question can never trigger a reset,
// because a question is not silence.
//
// A stuck button cannot be told apart from a deliberate silent hold. The
// signals are identical and no scheme distinguishes them, so it is handled by
// cost instead: the access point times out back to the saved network, and
// nothing is erased on the way in.
#define RESET_TAPS 5
#define RESET_WINDOW_MS 3000
// The face starts showing the count from here, so the gesture cannot complete
// without warning.
#define RESET_TAPS_VISIBLE 3

// Below this, a press was not a question - nobody says anything in a fifth of
// a second - so the utterance is cancelled rather than ended. Without it each
// tap of the reset gesture would run start/end and cost a Groq STT call on a
// rate-limited tier, five per gesture, plus five round trips on the link that
// is this project's blocking problem.
#define SHORT_PRESS_MS 200

// Counting presses inside a window, in the one place both callers can share.
//
// There are two, and they never run at once: net_task counts them to *enter*
// provisioning while the device is working, and app_main counts them to
// *leave* it while it is provisioning - net_task does not exist yet at that
// point, it is created after the radio is up. Each keeps its own instance
// rather than sharing state, because the same run of taps must not be seen
// by both.
typedef struct {
    uint8_t taps;
    TickType_t first;
    bool was_down;
} tap_counter_t;

// Feed it the debounced button every loop. Returns how many presses are in the
// current run, or 0 once the window lapses.
static uint8_t tap_count(tap_counter_t *c, bool down, TickType_t now) {
    if (c->taps > 0 && (uint32_t)(now - c->first) * portTICK_PERIOD_MS > RESET_WINDOW_MS) {
        c->taps = 0;
    }
    if (down && !c->was_down) {  // the press edge, not the hold
        if (c->taps == 0) c->first = now;
        c->taps++;
    }
    c->was_down = down;
    return c->taps;
}

static void net_task(void *arg) {
    static uint8_t chunk[1024];
    bool held = false;
    tap_counter_t taps_in = {0};
    TickType_t press_start = 0;

    TickType_t busy_since = 0;

    while (true) {
        const bool down = s_button_down;
        const TickType_t now = xTaskGetTickCount();

        const uint8_t taps = tap_count(&taps_in, down, now);
        s_face_reset_pct = (taps >= RESET_TAPS_VISIBLE)
                               ? (uint8_t)((taps * 100u) / RESET_TAPS)
                               : 0;

        if (taps >= RESET_TAPS) {
            ESP_LOGW(TAG, "hold-to-reset completed; restarting into provisioning");
            // A hold in ST_LISTENING is also an utterance in flight. Abandon it
            // the way the interrupt path already does, so the server is not left
            // waiting on audio that will never arrive.
            if (esp_websocket_client_is_connected(s_ws)) {
                esp_websocket_client_send_text(s_ws, "{\"type\":\"cancel\"}", 17, SEND_TIMEOUT);
            }
            xStreamBufferReset(s_mic_buf);
            s_state = ST_IDLE;
            s_face_reset_pct = 0;

            config_request_provisioning();
            xEventGroupSetBits(s_wifi_events, WIFI_PROVISION_BIT);
            vTaskDelay(pdMS_TO_TICKS(100));  // let the cancel leave and the log flush
            esp_restart();
        }

        // Last-resort unwedge, measured from the last thing the server sent
        // rather than from the button press.
        //
        // A reply drains at playback speed, so a long answer legitimately
        // takes fifteen or twenty seconds from press to silence. Timing from
        // the press meant this fired in the middle of audio that was playing
        // perfectly well and cut it off. Only genuine silence from the server
        // counts as stuck.
        if (s_state == ST_IDLE) {
            busy_since = now;
            s_last_activity = now;
        } else if ((now - s_last_activity) > pdMS_TO_TICKS(STUCK_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "no data from server for %d s in state %d, forcing idle",
                     STUCK_TIMEOUT_MS / 1000, (int)s_state);
            s_reply_finished = false;
            s_state = ST_IDLE;
            busy_since = now;
            s_last_activity = now;
        }
        (void)busy_since;

        if (down != held) {
            held = down;

            if (held) press_start = now;

            if (held && (s_state == ST_SPEAKING || s_state == ST_THINKING) &&
                esp_websocket_client_is_connected(s_ws)) {
                // A press during a reply means "stop, I want to ask again".
                //
                // Silence the speaker first and tell the server second: the
                // user should hear the interruption immediately, not after a
                // round trip. The spec rules out barge-in, but that is about
                // detecting speech during playback, which needs echo
                // cancellation. A deliberate button press needs none.
                ESP_LOGI(TAG, "interrupted while in state %d", (int)s_state);
                // Flag it rather than calling face_startle here: face_task
                // owns the animation, and this is a different task. It also
                // has to be recorded now, because a moment later the state
                // will be ST_IDLE and the reason for the change will be gone.
                s_face_startle = true;
                s_abort_playback = true;
                // Everything still in flight for this reply is now unwanted.
                s_discard_audio = true;
                s_discarded_bytes = 0;
                esp_websocket_client_send_text(s_ws, "{\"type\":\"cancel\"}", 17,
                                               SEND_TIMEOUT);
                // Let audio_out mute and drain before the new utterance opens.
                vTaskDelay(pdMS_TO_TICKS(40));
                s_state = ST_IDLE;
            } else if (held && !esp_websocket_client_is_connected(s_ws)) {
                // The keepalive can take the better part of a minute to notice
                // a link that died quietly, and every press until then vanishes
                // without a trace. From the outside that is indistinguishable
                // from broken hardware, so it must at least be visible here.
                ESP_LOGW(TAG, "press ignored: socket is down, reconnecting");
            }

            if (held && s_state == ST_IDLE && esp_websocket_client_is_connected(s_ws)) {
                xStreamBufferReset(s_mic_buf);
                adpcm_reset();
                s_dropped_blocks = 0;
                s_sent_bytes = 0;
                s_send_failures = 0;
                s_slowest_send = 0;
                s_audio_level = 0;  // do not open on the last reply's loudness
                s_state = ST_LISTENING;
                s_last_activity = now;
                // RSSI at the moment of the press, to separate a weak or noisy
                // link from a supply that sags once the radio starts working.
                wifi_ap_record_t ap;
                if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                    ESP_LOGI(TAG, "rssi %d dBm, channel %d", ap.rssi, ap.primary);
                }
                esp_websocket_client_send_text(
                    s_ws, "{\"type\":\"start\",\"codec\":\"adpcm\"}", 32, SEND_TIMEOUT);
                ESP_LOGI(TAG, "listening");
            } else if (!held && s_state == ST_LISTENING) {
                // Flush whatever is still buffered before closing the utterance.
                size_t got;
                while ((got = xStreamBufferReceive(s_mic_buf, chunk, sizeof(chunk), 0)) > 0) {
                    esp_websocket_client_send_bin(s_ws, (char *)chunk, got, SEND_TIMEOUT);
                }
                // A press too short to have said anything is not a question -
                // it is a miss, or one tap of the reset gesture. Cancelling
                // costs the server nothing; ending would spend an STT call on
                // a fifth of a second of room tone, five times per gesture.
                const uint32_t press_ms = (uint32_t)(now - press_start) * portTICK_PERIOD_MS;
                if (press_ms < SHORT_PRESS_MS) {
                    esp_websocket_client_send_text(s_ws, "{\"type\":\"cancel\"}", 17, SEND_TIMEOUT);
                    s_state = ST_IDLE;
                    s_last_activity = now;
                    ESP_LOGI(TAG, "press of %lu ms: cancelled, not a question",
                             (unsigned long)press_ms);
                } else {
                    esp_websocket_client_send_text(s_ws, "{\"type\":\"end\"}", 14, SEND_TIMEOUT);
                    s_state = ST_THINKING;
                    s_last_activity = now;
                    ESP_LOGI(TAG, "thinking: sent %lu B (%.1f s), %lu dropped, %lu failures, slowest send %lu ms, heap %lu",
                             (unsigned long)s_sent_bytes, s_sent_bytes / 32000.0f,
                             (unsigned long)s_dropped_blocks, (unsigned long)s_send_failures,
                             (unsigned long)(s_slowest_send * portTICK_PERIOD_MS),
                             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
                    wifi_ap_record_t ap_after;
                    if (esp_wifi_sta_get_ap_info(&ap_after) == ESP_OK) {
                        // A drop between press and release points at the radio
                        // losing ground while transmitting, not at distance.
                        ESP_LOGI(TAG, "rssi after upload: %d dBm", ap_after.rssi);
                    }
                }
            }
        }

        // Stream while the button is held: this removes upload time from the
        // latency budget instead of sending the whole utterance on release.
        if (s_state == ST_LISTENING) {
            size_t got = xStreamBufferReceive(s_mic_buf, chunk, sizeof(chunk), pdMS_TO_TICKS(10));
            if (got > 0) {
                const TickType_t t0 = xTaskGetTickCount();
                int rc = esp_websocket_client_send_bin(s_ws, (char *)chunk, got, SEND_TIMEOUT);
                const uint32_t took = (uint32_t)(xTaskGetTickCount() - t0);
                if (took > s_slowest_send) s_slowest_send = took;
                // A single 1 KB frame should leave in a few milliseconds. When
                // it does not, the uplink is the bottleneck and the microphone
                // buffer behind it is about to overflow.
                if (took > pdMS_TO_TICKS(200)) {
                    ESP_LOGW(TAG, "send of %u B took %lu ms", (unsigned)got,
                             (unsigned long)(took * portTICK_PERIOD_MS));
                }
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

    // The panel is optional, and it is asked about before WiFi so there is a
    // face to watch while the radio associates.
    //
    // Nothing below may be allowed to stop the device. It worked for two days
    // without a display and has to keep working without one: if nothing
    // answers on the bus, say so once and never start the task.
    // What the face costs, printed by the device rather than estimated. The
    // estimate was 8 KB and it was wrong by a factor of three, which is the
    // usual outcome of estimating on this project.
    const uint32_t heap_before_face = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT);

    if (ssd1306_init(&s_panel, PIN_SDA, PIN_SCL, FACE_I2C_HZ) == ESP_OK) {
        s_have_panel = true;
        xTaskCreate(face_task, "face", 3072, NULL, 2, NULL);
    } else {
        ESP_LOGW(TAG, "no OLED on SDA %d / SCL %d - running without a face",
                 (int)PIN_SDA, (int)PIN_SCL);
    }

    {
        const uint32_t after = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        ESP_LOGI(TAG, "heap %lu B before the face, %lu after: the face costs %ld",
                 (unsigned long)heap_before_face, (unsigned long)after,
                 (long)heap_before_face - (long)after);
    }

    // Ahead of wifi_start(), which waits for a network with no timeout. Until
    // now the button task was created after it, so while the device sat there
    // unable to connect, nothing sampled the button - the press was
    // undetectable in exactly the situation that needs it. It touches only
    // GPIO and its own debounce state, so it has no dependency on the radio.
    xTaskCreate(button_task, "button", 2048, NULL, 6, NULL);

    // Before anything that touches the radio, including provisioning.
    wifi_init_stack();

    device_config_t cfg;
    config_load(&cfg);

    // Two ways in: nothing is configured, or the hold-to-reset gesture asked
    // for it before restarting. Note what is deliberately absent - failing to
    // connect is not one of them. A device that knows a network waits for it,
    // because a router rebooting is worth waiting out and an access point that
    // appeared on its own would turn a two-minute outage into a device that
    // had stopped being a voice companion.
    const bool asked = config_take_provisioning_request();
    if (pl_decide(config_is_provisioned(&cfg), asked, false) == PL_MODE_PROVISION) {
        if (provision_start() == ESP_OK) {
            // Four ways out: a successful trial plus its grace window, five
            // minutes with nobody using the page, the same five taps that got
            // here, or provisioning stopping on its own. Only the first is the
            // happy one.
            //
            // The taps are counted here rather than in net_task because
            // net_task does not exist yet - it is created once the radio is
            // up, which is after this returns.
            tap_counter_t taps_out = {0};
            uint8_t shown = 0;
            while (provision_is_active() && !provision_complete() && !provision_idle_expired()) {
                const uint8_t taps = tap_count(&taps_out, s_button_down, xTaskGetTickCount());
                if (taps >= RESET_TAPS) {
                    ESP_LOGW(TAG, "five taps: leaving setup without configuring");
                    break;
                }
                // Same warning the entry gesture gives, in the only place this
                // screen has for it: a gesture that fires with no notice is
                // exactly what the countdown exists to prevent.
                if (taps >= RESET_TAPS_VISIBLE && taps != shown) {
                    provision_set_status(SS_STATUS_LEAVING);
                    shown = taps;
                } else if (taps == 0 && shown != 0) {
                    provision_set_status(SS_STATUS_WAITING);
                    shown = 0;
                }
                vTaskDelay(pdMS_TO_TICKS(20));  // fast enough not to miss a tap
            }
            provision_stop();
            config_load(&cfg);  // pick up whatever was just saved
        } else {
            // Nothing else to try. Fall through and attempt whatever is
            // configured: with no credentials that waits forever, which is at
            // least visible on the panel rather than a silent reboot loop.
            ESP_LOGE(TAG, "provisioning would not start");
        }
    }

    wifi_connect(cfg.ssid, cfg.pass);
    ws_start(cfg.uri);

    xTaskCreate(audio_in_task, "audio_in", 4096, NULL, 5, NULL);
    xTaskCreate(audio_out_task, "audio_out", 4096, NULL, 5, NULL);
    xTaskCreate(net_task, "net", 4096, NULL, 4, NULL);
    xTaskCreate(led_task, "led", 2048, NULL, 2, NULL);
    xTaskCreate(link_task, "link", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "ready - hold the button on GPIO%d and speak%s", PIN_BUTTON,
             s_have_panel ? "" : " (no display)");
}
