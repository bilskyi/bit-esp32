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
#include "settings_menu.h"
#include "settings_screen.h"
#include "setup_screen.h"
#include "secrets.h"
#include "ssd1306.h"

#define PIN_BCLK GPIO_NUM_4
#define PIN_WS GPIO_NUM_5
#define PIN_DIN GPIO_NUM_6
#define PIN_DOUT GPIO_NUM_7
#define PIN_MUTE GPIO_NUM_10
#define PIN_BUTTON GPIO_NUM_3
// The second button, wired to ground exactly like the first.
//
// GPIO 20 is U0RXD, and it is free only because the console is not on UART0 -
// CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y. On a board with a CP2102 or CH340
// bridge this pin is driven by the bridge and a button on it would fight an
// output. It is not a strapping pin, so a press during reset is harmless.
#define PIN_BUTTON_B GPIO_NUM_20
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
// Debounced second button, owned by button_task alongside the first.
static volatile bool s_button_b_down = false;
// The settings carousel. Seeded once in app_main, before face_task exists.
// After that face_task is its only writer here, and it is also the only task
// that draws, so the screen can never disagree with the state behind it.
//
// Task 6 needed two more things out of this struct for the audio path - the
// playback gain and the request for a demonstration tone - and kept the rule
// rather than bending it. Neither is read from here by another task:
// face_task publishes both below, each in the shape it actually has. So this
// is still one writer, and there is still no lock.
//
// Deliberately not volatile. settings_tick() takes a plain settings_t *, so a
// volatile struct could only be passed to it by casting the qualifier away at
// every call, which buys the appearance of safety and none of it. Instead the
// facts other tasks need are published separately, below: each written by one
// task, and the sharing visible in the declaration rather than inferred from
// a struct several tasks happen to reach into.
static settings_t s_settings;

// Whether the menu is up, published by face_task for the tasks that must not
// mistake a press meant for the menu for a question.
//
// volatile because net_task, boot_gesture_task and app_main read it in loops
// they do not write it from, and without the qualifier the compiler is
// entitled to hoist the read out of those loops. It happens not to today -
// the address escapes to other translation units, every iteration passes
// through an external call, the part is unicore, and the build is -Og with no
// LTO - but not one of those reasons is recorded in the object code, and a
// switch to -Os or IPO could take any of them away silently. The gate that
// keeps the menu out of the conversation depends on this read being fresh.
static volatile bool s_menu_open = false;

// Unity in Q15. settings_menu.h guarantees this is exactly what the top
// volume step returns, so this is a documented contract rather than a
// duplicated constant - and it is named because two places depend on it: the
// initialiser just below, and the passthrough in audio_out_task.
#define PLAY_GAIN_UNITY 0x7fff

// The Q15 playback gain the volume page is currently showing, published by
// face_task for audio_out_task to scale the reply with.
//
// A fact rather than an event: whoever reads it wants the value that is true
// now, and int32_t is naturally aligned on this part, so the read is a single
// load and cannot tear. Publishing it keeps settings_menu.h out of the audio
// path entirely, and keeps s_settings to one reader as well as one writer.
//
// Seeded in app_main immediately after settings_init(), not only here and not
// in face_task. On a device whose OLED does not answer, face_task is never
// created - so nothing would ever publish, and every reply would be
// multiplied by whatever this initialiser says. The initialiser is unity, the
// value settings_menu.h guarantees for the top step, so even that failure
// sounds exactly like the device did before this task existed. Zero would
// have been a permanently, silently muted device with no symptom but silence,
// which is the one outcome worth engineering against here.
static volatile int32_t s_play_gain = PLAY_GAIN_UNITY;

// A tone the volume page has asked for: raised by face_task, cleared by
// audio_out_task when it plays one.
//
// An event rather than a fact, and it crosses in the opposite direction to
// s_play_gain, which is why it is a flag of its own instead of a second read
// of s_settings. face_task clears s_settings.beep_requested itself and raises
// this, so the struct keeps its single writer. The same shape as
// s_abort_playback and s_reply_finished below, for the same reason.
//
// The rule this exists to keep: the last thing you hear is the level you
// settled on. That is the whole reason the page makes a sound, and neither
// obvious ordering manages it. A tone occupies about 210 ms once the drain is
// counted, while B debounces at 25 ms and the menu ticks every 40, so a tap
// landing inside a tone is the ordinary case rather than the awkward one.
//
// Clearing the flag *before* the tone queues them: a brisk walk stacks up
// blips that lag the panel, and because the gain is read when a tone starts
// rather than when the tap happened, they come out at whatever the newest
// level is by then - several identical ones, arriving after the user has
// stopped pressing.
//
// Clearing it *after* the tone throws away every tap that arrived during one,
// so a quick double-tap plays the level you passed through and never the one
// you stopped on. Which breaks the rule in the other direction.
//
// So audio_out_task does neither on its own: it clears, plays, and looks
// again. Taps during a tone coalesce into one re-raise, and the tone that
// answers them reads s_play_gain as it *starts*. Queue depth one, always at
// the newest level, and a trailing tone that can never announce a level the
// user has already left - if they have walked on to muted by then it is
// silent, which is exactly the value being demonstrated.
static volatile bool s_beep_pending = false;

// Set by net_task when the user presses during a reply; audio_out acts on it.
static volatile bool s_abort_playback = false;
// True while the speaker is actually producing sound. Owned by audio_out_task.
//
// The interrupt used to key off s_state alone, and s_state can be wrong: the
// stuck-state timer forces ST_IDLE after twenty seconds of server silence, and
// the log caught audio starting three milliseconds after it did. A press then
// found ST_IDLE, took the "start a new utterance" path instead of the
// interrupt path, and the abandoned reply carried on talking over the new
// question. Whether sound is coming out is a fact; what the state machine
// believes is an opinion.
static volatile bool s_playing = false;
// Set when a reply is cancelled, cleared by the "speaking" that opens the next
// one - not by the cancelled reply's own "done", which arrives too late to be
// a boundary. See the handler for why.
//
// Resetting the play buffer only discards what has already arrived. The server
// runs up to playback_lead_s ahead of real time - four seconds - so the rest of
// a cancelled reply is still in flight, and without this it lands in the buffer
// a moment later, the amp comes back on, and the device carries on talking
// after being told to stop.
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

    // Both buttons in one call: they want the identical mode, pull and
    // interrupt setting, and a second config with the same body is a second
    // place for them to drift apart.
    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << PIN_BUTTON) | (1ULL << PIN_BUTTON_B),
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
        // provision_start() brings the station interface up too - Task 5's
        // scan and trial need it - and says so explicitly: "esp_wifi_connect()
        // is deliberately not called" there. This handler used to ignore that
        // and connect anyway, because WIFI_EVENT_STA_START does not say who
        // asked for the station to start. The result was the station
        // hammering the old, now-unreachable network in the background for
        // the entire time the access point was up - on the same radio that
        // was supposed to be free for esp_wifi_scan_start() and for the AP's
        // own beacons. That is what made /scan intermittently report "busy"
        // (ESP_ERR_WIFI_STATE, ready to try the last network in the middle
        // of a scan) and what most likely explains a captive-portal page that
        // sometimes loads and sometimes times out.
        if (!provision_is_active()) {
            esp_wifi_connect();
        }
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
        if (provision_is_active()) {
            // Same reasoning as WIFI_EVENT_STA_START above: nothing here
            // should be trying to rejoin the old network while setup is up.
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

    // app_main seeded s_settings from NVS before this task was created, so
    // the two settings that are visible from outside can be applied here,
    // before the first frame is drawn, rather than a frame or two into the
    // boot animation.
    face_set_resting(&s_face, settings_eyes_emotion(s_settings.step[SETTINGS_PAGE_EYES]));
    // The contrast write's result is discarded on purpose, here and in the
    // loop, while ssd1306_flush()'s is checked and logged. Not an oversight
    // and not a double standard: the only way this write fails is a panel
    // that has stopped answering, and the flush a few lines below will fail
    // in the same frame and say so - once on the way down and once on the way
    // back, which is the whole point of the way it says it. A second error
    // path here would report the same fact in a way that does not know it is
    // the same fact.
    (void)ssd1306_set_contrast(&s_panel,
                               settings_screen_contrast(s_settings.step[SETTINGS_PAGE_SCREEN]));
    uint8_t shown_screen_step = s_settings.step[SETTINGS_PAGE_SCREEN];

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

        // Either button. The face's reaction to a press is "something is
        // being asked of me", and that is as true of the one that opens the
        // menu as of the one that records a question.
        const bool down = s_button_down || s_button_b_down;
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

        // The menu is driven from here because this task already ticks at a
        // fixed 40 ms and already owns the panel. 40 ms against a 25 ms
        // debounce and a 1 s hold has room to spare.
        //
        // Except while provisioning. That screen is showing an access point
        // name, an address and a code that someone is copying into a phone,
        // and the menu would cover all three. The gesture is starved of
        // input rather than the menu hidden at the moment it would be drawn:
        // a menu that opened behind the setup screen and then appeared when
        // provisioning ended would be worse than either, and hiding it would
        // also leave B meaning two things at once once Task 7 gives that
        // screen its own B hold for the way out.
        //
        // Starved, not skipped, so the machine keeps ticking: a menu that was
        // already open when provisioning started still has its twenty-second
        // idle timeout, and that is what closes it.
        const bool menu_input = !provision_is_active();
        settings_tick(&s_settings, s_button_down && menu_input,
                      s_button_b_down && menu_input, t);

        // Publish what the other tasks need, from the task that owns the
        // state, immediately after the tick that can change it.
        s_menu_open = s_settings.open;
        // Unconditionally, every frame, rather than only on a change: the
        // write is a single aligned store and comparing first would cost a
        // load to save it. And before the beep is handed on below - the tap
        // that asks for a tone is the same tap that changed the step, and a
        // tone at the level before it demonstrates nothing.
        s_play_gain = settings_volume_gain(s_settings.step[SETTINGS_PAGE_VOLUME]);

        // Hand the tone to the task that owns the amplifier. Cleared here so
        // s_settings keeps its single writer, and raised as a flag of its own
        // so audio_out_task never has to reach into the struct - the whole
        // argument is at s_beep_pending's declaration.
        //
        // No check for whether a reply is playing: audio_out_task knows that
        // and this task does not. Nothing here can tell whether s_playing
        // will still be true by the time the flag is read.
        if (s_settings.beep_requested) {
            s_settings.beep_requested = false;
            s_beep_pending = true;
        }

        // Brightness is applied as it changes rather than on the way out, so
        // the value can be judged by looking at it. Only on a change: the
        // contrast command is an I2C transaction, and one per frame would
        // cost a write the flush below has spent effort avoiding.
        if (s_settings.step[SETTINGS_PAGE_SCREEN] != shown_screen_step) {
            shown_screen_step = s_settings.step[SETTINGS_PAGE_SCREEN];
            // Discarded for the reason given at the top of this task.
            (void)ssd1306_set_contrast(&s_panel, settings_screen_contrast(shown_screen_step));
        }

        if (s_settings.wifi_requested) {
            s_settings.wifi_requested = false;
            // This path does not save. Anything changed on the volume,
            // brightness or eyes pages during this visit is lost across the
            // restart, because leaving through the WiFi page is not leaving
            // through the exit hold and only the exit hold raises
            // save_requested. Defensible - you came to this page to redo the
            // network, not to keep a volume change - but not obvious, so it
            // is written down here and on the bench sheet rather than being
            // discovered and filed as a bug.
            ESP_LOGW(TAG, "settings: wifi setup requested, restarting into provisioning");
            config_request_provisioning();
            vTaskDelay(pdMS_TO_TICKS(100));  // let the log line reach the console
            esp_restart();
        }

        if (s_settings.save_requested) {
            s_settings.save_requested = false;
            // The commit below is a flash write, which on this part means the
            // instruction cache is disabled for its duration: every task
            // running from flash stalls for it, not just this one. Tens of
            // milliseconds, once per menu close, so the cost is a dropped
            // frame and possibly a brief audible artefact if a reply happens
            // to be playing - worth it for a write that happens on the way
            // out instead of on every press.
            face_set_resting(&s_face, settings_eyes_emotion(s_settings.step[SETTINGS_PAGE_EYES]));
            const esp_err_t serr = config_save_settings(s_settings.step[SETTINGS_PAGE_VOLUME],
                                                        s_settings.step[SETTINGS_PAGE_SCREEN],
                                                        s_settings.step[SETTINGS_PAGE_EYES]);
            if (serr != ESP_OK) {
                // The values are live and already applied; losing them at the
                // next boot is worth less than making a reboot of it.
                ESP_LOGW(TAG, "settings: nvs write failed (%s)", esp_err_to_name(serr));
            } else {
                ESP_LOGI(TAG, "settings: saved vol=%u bright=%u eyes=%u",
                         s_settings.step[SETTINGS_PAGE_VOLUME],
                         s_settings.step[SETTINGS_PAGE_SCREEN],
                         s_settings.step[SETTINGS_PAGE_EYES]);
            }
        }

        // One task draws, and it chooses which of the three things to draw.
        //
        // setup_screen.c renders into a buffer it is handed and never touches
        // the panel, so this is the only writer either way and no lock is
        // needed. The setup screen gets a buffer of its own rather than
        // borrowing face_t's private one: reaching into another module's
        // state is exactly what its no-dependency rule exists to prevent.
        // 1 KB of BSS, not heap.
        //
        // settings_screen.c has the same contract, and shares the same
        // buffer rather than taking a second kilobyte: the setup screen and
        // the settings menu cannot be up at the same instant, because only
        // one of these arms runs per frame.
        static uint8_t s_setup_fb[FACE_FB_BYTES];
        const uint8_t *fb;
        if (s_settings.open) {
            settings_screen_render(s_setup_fb, &s_settings);
            fb = s_setup_fb;
        } else if (provision_is_active()) {
            setup_screen_t s;
            provision_screen(&s);
            setup_screen_render(s_setup_fb, &s);
            fb = s_setup_fb;
        } else {
            // The bar across the eyes is now the menu's hold, not the
            // five-tap count: it is the warning that a hold on B is about to
            // take the panel away from the face, given while there is still
            // time to let go.
            face_set_reset_progress(&s_face, s_settings.hold_pct, t);
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
        //
        // Deliberately the raw button, not button_outside_menu(). This is the
        // one reader outside the menu that is not gated, so the reason has to
        // be the strong one rather than the obvious one.
        //
        // A mask here would be a *second* mask, latched independently of
        // net_task's. The two can disagree - they are lifted by whichever
        // loop next sees the button released, and these loops do not run
        // together - and a disagreement in this direction stops the
        // microphone feeding while net_task still believes the utterance is
        // live. It then sends "end" on silence and spends an STT call on
        // nothing, which is the very cost the gate was added to avoid.
        //
        // The obvious reason is real but small: this is a level test guarded
        // by ST_LISTENING, the only way that state is reached with the menu
        // up is an utterance net_task is already about to end, and the audio
        // in that gap belongs to the question the release path is flushing.
        // In the ordinary path that gap is one 10 ms iteration; it is only
        // large when net_task is stuck in a send.
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

// A short blip at the level the volume page is showing, so the setting can be
// judged by ear. A volume control you cannot hear while setting it is a guess.
//
// It is played from audio_out_task and nowhere else. The amplifier's mute pin
// belongs to that task, the I2S channel is created once at boot and never
// re-initialised - re-init is what pops - and the drain before muting is what
// keeps the tail of a word. Reaching around all three from the menu's own
// context is how the pop comes back.
#define BEEP_HZ 660
#define BEEP_MS 150
#define BEEP_RAMP_MS 20
// About -12 dBFS before the volume gain: the same headroom playback_main.c
// chose for the bring-up tone, for the same reason. Loud enough to judge
// across a room, quiet enough that a mistake does not arrive at full scale.
#define BEEP_AMPLITUDE 8000

// `frame` is the caller's own I2S staging buffer, borrowed rather than
// duplicated: this runs on audio_out_task, nothing else can be using it, and
// a second static copy would cost 4 KB of BSS on a part whose free heap fell
// to 30 KB the moment wss:// was switched on.
static void play_beep(int32_t *frame, int32_t gain) {
    if (gain == 0) return;  // at muted, silence is the value being demonstrated

    const int total = (SAMPLE_RATE * BEEP_MS) / 1000;      // 2400 samples
    const int ramp = (SAMPLE_RATE * BEEP_RAMP_MS) / 1000;  // 320 samples
    // 16000 / 660 truncates to 24, which is 666.7 Hz rather than 660. Nobody
    // can hear that, and a whole number of samples per period is what lets
    // the phase be the sample index and nothing else - no accumulator to
    // carry across a block boundary, and no drift for it to accumulate.
    const int period = SAMPLE_RATE / BEEP_HZ;

    amp_enable(true);
    for (int done = 0; done < total; done += BLOCK_SAMPLES) {
        const int n = (total - done < BLOCK_SAMPLES) ? (total - done) : BLOCK_SAMPLES;
        for (int i = 0; i < n; i++) {
            const int at = done + i;

            // The envelope, in Q8: up across the first `ramp` samples, down
            // across the last. It reaches exactly zero at both ends - at == 0
            // gives 0, and at == total - 1 gives 256 / 320, which truncates
            // to 0 - so the waveform starts and ends at silence. That is the
            // whole point of it: a tone that begins at full amplitude is a
            // step, and a step is a click. The same argument as fill_tone()
            // in playback_main.c, in integers instead of floats.
            int32_t env = 256;
            if (at < ramp) env = (at * 256) / ramp;
            else if (at > total - ramp) env = ((total - at) * 256) / ramp;

            // A triangle in Q8, from -256 up to +256 and back. At 660 Hz
            // through a speaker this small it is indistinguishable from a
            // sine, and it needs no table, no float and no <math.h>.
            //
            // Both halves span the full 512 counts, which is why the 4 is
            // there. Half of that - the first draft - climbs from -256 only
            // as far as 0, jumps to +256, falls back to 0 and jumps to -256
            // again: two sawtooths with a full-scale discontinuity twice per
            // period. That is a buzz, not a tone, and it would have been
            // heard before it was read.
            const int p = at % period;
            const int tri = (p < period / 2)
                                ? (p * 4 * 256) / period - 256
                                : 256 - ((p - period / 2) * 4 * 256) / period;

            // The widest value here is not one of the three products - it
            // is the I2S slot itself, 8000 << 16 = 524,288,000, a quarter of
            // what int32_t holds. The products are all smaller, the largest
            // being 8000 * 32767 = 262,136,000, so none of them needs a
            // wider type either.
            int32_t v = (BEEP_AMPLITUDE * tri) >> 8;
            v = (v * env) >> 8;
            v = (v * gain) >> 15;
            frame[i * 2] = v << 16;
            frame[i * 2 + 1] = 0;
        }
        size_t written = 0;
        i2s_channel_write(s_tx, frame, (size_t)n * 2 * sizeof(int32_t), &written,
                          portMAX_DELAY);
    }
    // The same drain the end of a reply uses, and deliberately the same
    // constant rather than a second one: i2s_channel_write returns once the
    // samples are in the DMA ring, not once they have been heard, so cutting
    // the amp on that boundary would truncate the fade-out back into the
    // click the fade-out exists to avoid.
    vTaskDelay(pdMS_TO_TICKS(DRAIN_MS));
    amp_enable(false);
}

static void audio_out_task(void *arg) {
    // The play buffer now holds compressed audio, so the same RAM rides out
    // four times as long a gap: 48 KB is about six seconds of speech.
    static uint8_t coded[BLOCK_SAMPLES / 2];
    static int16_t pcm[BLOCK_SAMPLES];
    static int32_t frame[BLOCK_SAMPLES * 2];
    // The gain actually in force at the last sample written. Carried across
    // blocks so a change made mid-reply is a ramp rather than a step.
    //
    // The initialiser is defensive only. Every reply re-seeds this at its
    // first block, below, so nothing depends on what it starts at - but a
    // plausible value costs nothing and an uninitialised one would be a
    // multiplier on the first thing anyone hears.
    int32_t gain = s_play_gain;
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
            s_playing = false;
            s_reply_finished = false;
            s_abort_playback = false;
            ESP_LOGI(TAG, "playback aborted");
            continue;
        }

        // The tone the volume page asked for. Here, above the receive, because
        // this is the one point in the loop where the amplifier is idle and
        // this task is not holding samples it already owes the speaker.
        //
        // A loop rather than an if, with the clear at the top of it: that is
        // what makes the queue exactly one deep. Every tap arriving during a
        // tone re-raises the flag and they fold into each other; when the tone
        // ends, one more plays, and it reads s_play_gain as it starts - so it
        // says where the user stopped, not what asked for it. The argument in
        // full is at the flag's declaration.
        //
        // Nothing here can lose a raise, and the guarantee is the ordering
        // rather than the scheduling: the flag is cleared *before* play_beep,
        // so any raise from that instant onwards is still standing when the
        // loop tests again. That holds however the two tasks happen to be
        // scheduled against each other. face_task's priority 2 against this
        // task's 5 means it cannot preempt the gap between play_beep returning
        // and the test at all - but that is a second reason, not the one being
        // relied on, so changing either priority cannot reopen the question. A
        // tap arriving after the final test waits for the next pass of the
        // outer loop, 20 ms later.
        //
        // Nor can it run away. Nothing in play_beep touches the flag, so every
        // extra pass costs a fresh debounced press inside the previous tone.
        // Stop pressing, and at most one more tone plays. Never two.
        while (s_beep_pending) {
            s_beep_pending = false;
            // Not over a reply: if audio is already coming out, the reply is
            // the demonstration. s_playing cannot change under this test -
            // this task is the only thing that writes it, and the beep runs
            // to completion here before the loop can turn it on.
            if (!s_playing) {
                play_beep(frame, s_play_gain);
            }
        }

        size_t got = xStreamBufferReceive(s_play_buf, coded, sizeof(coded), pdMS_TO_TICKS(20));

        if (got > 0 && s_discard_audio) {
            // Stale by definition, and this is the one place that can catch
            // it. The receive handler checks the same flag, but a send that
            // was already blocked inside xStreamBufferSend waiting for room
            // has passed that check: the abort frees the room, the send wakes
            // and lands audio in the buffer that was just cleared. Measured -
            // "playback aborted" and "playing" in the same millisecond, then
            // 4.2 s of the abandoned reply. It only happened when the buffer
            // had filled, which is why it was intermittent.
            s_discarded_bytes += (uint32_t)got;
            continue;
        }

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
                // Start this reply at the level the menu is showing rather
                // than ramping to it from wherever the last one ended. The
                // ramp below is for a change made *during* a reply; used here
                // it would put 32 ms of the previous setting at the front of
                // the new one - which after "set it to muted" is a burst of
                // full-volume speech, the one thing that setting exists to
                // prevent.
                gain = s_play_gain;
                amp_enable(true);
                playing = true;
                s_playing = true;
                s_play_dropped = 0;
                ESP_LOGI(TAG, "playing");
            }
            const size_t n = adpcm_decode_block(coded, got, pcm);

            // This is why the face needed no mouth: the reply's own loudness
            // squashes the eyes on every syllable, from the samples that are
            // about to be played rather than from a guess about timing.
            //
            // Before the volume scaling, deliberately. The eyes squash on the
            // reply's own syllables; measure after the gain and the face goes
            // still at low volume, saying the assistant is mumbling when it is
            // only quiet - and stops moving altogether at muted, where the
            // face is the only thing left to show anything is happening.
            s_audio_level = block_level(pcm, n);

            // Apply the volume setting, ramping to it across the block instead
            // of stepping between two samples. A step in the coefficient is a
            // step in the waveform, and a step is a click - the same reason
            // fill_tone() in playback_main.c ramps its ends.
            //
            // The ramp lands exactly on `want` at the last sample, and `gain`
            // carries that into the next block, so the level is continuous
            // across a block boundary as well as within one. One block is
            // 32 ms, which is fast enough that a tap on B is heard as an
            // immediate change and slow enough that no edge survives it.
            //
            // All of this fits in int32_t, so nothing here calls into libgcc
            // for a 64-bit divide on the audio path: both gains are Q15 and so
            // at most 32767, i is at most BLOCK_SAMPLES - 1 = 511, and
            // 32767 * 511 = 16,743,937. The sample product is wider at
            // 32768 * 32767 = 1,073,709,056, but the widest value on this path
            // is neither - it is the slot itself, 32767 << 16 = 2,147,418,112,
            // which leaves 65,535 counts, 0.003% of the type. Tight, and
            // deliberately still tighter than the line it replaced: the shift
            // by 15 clamps v to +-32767, where the old (int32_t)pcm[i] << 16
            // could reach INT32_MIN exactly. The passthrough below is that old
            // line, so it is the one case that still touches the end stop.
            const int32_t want = s_play_gain;

            if (want == gain && want == PLAY_GAIN_UNITY) {
                // The top step is supposed to leave the reply alone, and
                // (pcm * 32767) >> 15 does not: it alters exactly half of all
                // int16 inputs by one count and carries half a count of DC
                // with it. Inaudible at -90 dBFS, and forced by a Q15 table
                // whose top entry cannot be 32768 - but the design document
                // promises "a device nobody configures sounds exactly as it
                // does today", and this is the step such a device runs at. So
                // that promise is kept literally, here, rather than approxi-
                // mately: at a constant top-step gain the samples are copied
                // through untouched.
                //
                // It is also the cheap path on the setting most devices will
                // sit at - 512 multiply-and-shift pairs a block that no longer
                // happen. The general path below covers every other gain, and
                // every block where the gain is still moving, including a ramp
                // that ends at unity.
                for (size_t i = 0; i < n; i++) {
                    // int16 into the top half of a 32-bit slot; left channel
                    // only, which is what the amplifier selects with SD high.
                    frame[i * 2] = (int32_t)pcm[i] << 16;
                    frame[i * 2 + 1] = 0;
                }
            } else {
                // n is two samples per coded byte and so always even, never 1.
                // The guard makes a single-sample block non-fatal rather than
                // correct: it would avoid the division by zero, but that
                // sample would play at the old gain and the next block would
                // start at the new one - an unramped step, which is exactly
                // the click the ramp exists to prevent. It is here so that a
                // future change to the decoder cannot crash the audio task
                // while someone works out what it should sound like.
                const int32_t last = (n > 1) ? (int32_t)(n - 1) : 1;
                for (size_t i = 0; i < n; i++) {
                    const int32_t g = gain + ((want - gain) * (int32_t)i) / last;
                    const int32_t v = ((int32_t)pcm[i] * g) >> 15;
                    frame[i * 2] = v << 16;
                    frame[i * 2 + 1] = 0;
                }
            }
            gain = want;
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
            s_playing = false;
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
            // A "done" with nothing playing. Which reply it closes decides
            // whether this is a rescue or a fault, and the flag does not say.
            //
            // The rescue: a reply finished without producing a single byte of
            // audio - a TTS failure, most often. Nothing was playing, so the
            // branch above never runs, and without this the device would sit
            // in THINKING forever and silently ignore every button press.
            //
            // The fault: the device is not in a reply at all, so the "done"
            // closes one that is already over - the reply the user just
            // interrupted, or an utterance the server declined to answer. The
            // server sends "done" for those too, and it lands *after* the mic
            // has started on the next question: measured at 0.3 s twice and
            // 7 s twice. Forcing ST_IDLE then takes the state out from under
            // ST_LISTENING, and "end" is only ever sent from ST_LISTENING - so
            // the question is recorded, the release sends nothing, and the
            // server waits its full sixty seconds while the user hears
            // nothing at all. That is what the 28 Aug log caught: a start
            // accepted 0.19 s after a cancel, 0.22 s of audio, no "end".
            //
            // Clear it either way. Leaving a stale flag raised only moves the
            // damage to the moment the device next reaches THINKING.
            s_reply_finished = false;
            if (s_state == ST_THINKING || s_state == ST_SPEAKING) {
                s_state = ST_IDLE;
                ESP_LOGW(TAG, "idle (reply produced no audio)");
            } else {
                ESP_LOGI(TAG, "stale done ignored in state %d", (int)s_state);
            }
        }
    }
}

// Samples both buttons and nothing else, so their timing cannot be affected
// by a send that is waiting on the network. One task for two pins: the reason
// it exists in the first place is the same for both.
static void button_task(void *arg) {
    struct { gpio_num_t pin; bool stable; TickType_t changed; volatile bool *out; } b[2] = {
        {PIN_BUTTON, false, 0, &s_button_down},
        {PIN_BUTTON_B, false, 0, &s_button_b_down},
    };

    while (true) {
        const TickType_t now = xTaskGetTickCount();
        for (int i = 0; i < 2; i++) {
            const bool down = gpio_get_level(b[i].pin) == 0;
            if (down != b[i].stable && (now - b[i].changed) > pdMS_TO_TICKS(DEBOUNCE_MS)) {
                b[i].stable = down;
                b[i].changed = now;
                *b[i].out = down;
            }
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

// The button as everything outside the settings menu must see it: the
// debounced level, minus any press the menu has taken for itself.
//
// The menu is worked with A as much as with B - A walks the pages, a
// one-second A hold closes it - and to a task that reads s_button_down every
// one of those is a press like any other. Ungated, walking the carousel
// spends a start/cancel round trip per page, closing the menu by hand sends a
// second of room tone to be transcribed, and five taps inside three seconds
// reboot the device into provisioning and take the unsaved settings with it.
//
// Two things beyond "nothing while the menu is open" have to be true, and the
// latch is what makes the second of them true.
//
// Opening the menu part-way through an utterance arrives at net_task as a
// release, so the question ends through the path that already exists and the
// server gets its "end" rather than being left waiting. That is what the
// design asks for and it is better than a cancel.
//
// And the press that closed the menu is still physically down at the moment
// the menu goes away. Without the latch it would arrive as a fresh press edge
// - a start to net_task, a tap to the counters - the instant someone finished
// the gesture that means "I am done". So the mask outlives the menu and is
// lifted only by an actual release: exactly the remainder of that one
// physical press is discarded, not the button.
//
// Each caller keeps its own instance, the way each keeps its own
// tap_counter_t, because the mask records where one loop is in one physical
// press and two loops are not in the same place. Each polls at 20 ms or
// faster against a 25 ms debounce, so none can miss the release that lifts
// it - except net_task, which can sit in a send for up to SEND_TIMEOUT. A
// menu opened and closed entirely inside one such stall is never seen there
// at all; that fails safe, to the behaviour from before the menu existed.
//
// What this deliberately does not do is give the menu the interrupt. While it
// is open, A cannot stop a reply that is playing, because the interrupt path
// needs the press edge this removes. The reply is audible and the way to stop
// it is to close the menu first, which is a second's hold.
typedef struct {
    bool masked;
} menu_mask_t;

static bool button_outside_menu(menu_mask_t *m) {
    const bool raw = s_button_down;
    if (s_menu_open) {
        m->masked = true;
    } else if (!raw) {
        m->masked = false;
    }
    return raw && !m->masked;
}

// Watches for the same five-tap gesture while wifi_connect() below is still
// blocked waiting for a network. net_task is where the gesture normally
// lives, but it is not created until wifi_connect() returns - so a device
// that cannot reach its saved network had no way out at all: the wait has
// no timeout by design, and nothing was listening for the one thing that
// was supposed to end it early. That is the bug behind "it just sits there
// with its eyes shut and won't go into setup."
//
// Scoped tightly: created just before wifi_connect(), deleted right after it
// returns. It does not set WIFI_PROVISION_BIT and rely on wifi_connect()
// waking up on its own - esp_restart() below makes that moot, and racing
// app_main's wake-up against this task's own restart would risk falling
// through to ws_start() with no network for one extra boot cycle before the
// NVS request took effect. A clean reboot sidesteps that race rather than
// handling it.
static void boot_gesture_task(void *arg) {
    tap_counter_t taps = {0};
    // This task lives across wifi_connect(), which has no timeout - so it is
    // alive in exactly the situation where the panel starts advertising a way
    // into setup and someone starts pressing things. The menu can be open
    // here, and four presses round the carousel plus one would otherwise be
    // the reset gesture.
    menu_mask_t mask = {0};
    while (true) {
        const TickType_t now = xTaskGetTickCount();
        const uint8_t n = tap_count(&taps, button_outside_menu(&mask), now);
        s_face_reset_pct = (n >= RESET_TAPS_VISIBLE) ? (uint8_t)((n * 100u) / RESET_TAPS) : 0;
        if (n >= RESET_TAPS) {
            ESP_LOGW(TAG, "five taps while stuck connecting; restarting into provisioning");
            config_request_provisioning();
            vTaskDelay(pdMS_TO_TICKS(100));  // let the log line reach the console
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(20));  // fast enough not to miss a tap
    }
}

static void net_task(void *arg) {
    static uint8_t chunk[1024];
    bool held = false;
    menu_mask_t mask = {0};
    tap_counter_t taps_in = {0};
    TickType_t press_start = 0;

    TickType_t busy_since = 0;

    while (true) {
        // The button, minus anything the settings menu has taken. The whole
        // argument for the gate, and for the latch inside it, is above
        // button_outside_menu(); the short version is that A works the menu
        // as well as it works push-to-talk, and this task must not confuse
        // the two.
        const bool down = button_outside_menu(&mask);
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

            // s_playing is in here because s_state can be wrong. The
            // stuck-state timer forces ST_IDLE after twenty seconds of server
            // silence, and the log caught audio starting three milliseconds
            // later - a press then took the "new utterance" path and the
            // abandoned reply talked over the question being recorded. If
            // sound is coming out, a press stops it, whatever the state
            // machine currently believes.
            if (held && (s_state == ST_SPEAKING || s_state == ST_THINKING || s_playing) &&
                esp_websocket_client_is_connected(s_ws)) {
                // A press during a reply means "stop, I want to ask again".
                //
                // Silence the speaker first and tell the server second: the
                // user should hear the interruption immediately, not after a
                // round trip. The spec rules out barge-in, but that is about
                // detecting speech during playback, which needs echo
                // cancellation. A deliberate button press needs none.
                // Mute here, not in audio_out_task. Setting a flag and waiting
                // for the other task to notice costs a scheduling hop plus
                // however long it is blocked inside i2s_channel_write - which
                // is one DMA buffer, tens of milliseconds. This is a single
                // GPIO write and it happens on the press itself, so the
                // speaker goes quiet as fast as the button is debounced.
                // audio_out_task still does the rest of the teardown.
                amp_enable(false);
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

    // Read before the panel, not after it, and the menu is seeded here rather
    // than inside face_task.
    //
    // face_task runs at a higher priority than app_main, so it starts the
    // instant xTaskCreate returns and has drawn its first frame before the
    // next line of this function executes. Anything it reads at startup has
    // to be true *before* it is created, not a few milliseconds later:
    // seeding the menu from inside face_task would have read a zeroed config
    // every single time, and left the panel at its dimmest step and the
    // volume at muted with no way to notice but the symptom.
    //
    // Doing it here also means the values still arrive on a device with no
    // OLED, where face_task is never created at all but audio_out_task still
    // reads the volume.
    device_config_t cfg;
    config_load(&cfg);
    settings_init(&s_settings, cfg.volume, cfg.screen, cfg.eyes);
    // Publish the volume here too, not only from face_task. face_task
    // republishes it on every frame from now on - but face_task is not
    // created at all on a device whose OLED does not answer, and
    // audio_out_task is. Without this line that device would play every reply
    // at whatever s_play_gain's initialiser happens to say, with nothing on
    // the panel to explain it, because there is no panel.
    s_play_gain = settings_volume_gain(s_settings.step[SETTINGS_PAGE_VOLUME]);

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
            // The menu cannot *open* while this screen is up - face_task
            // starves the gesture of input for exactly as long as
            // provision_is_active() - so there is nothing here for the mask to
            // discard, with one exception: a menu that was already open when
            // provisioning started. That needs B held from power-on and is
            // very likely unreachable, since provisioning begins well inside
            // the two seconds the hold takes, but it was never ruled out. In
            // that case the mask is live and the five taps out of setup are
            // blocked until the menu's own twenty-second timeout closes it.
            // Self-correcting, and not worth code - but the next person
            // deciding whether this mask can be deleted needs to know it is
            // not decorative.
            //
            // It is used at all so that all three tap counters read the button
            // the same way; one of the three reading it differently is a trap
            // for whoever changes this next.
            menu_mask_t mask = {0};
            uint8_t shown = 0;
            while (provision_is_active() && !provision_complete() && !provision_idle_expired()) {
                const uint8_t taps =
                    tap_count(&taps_out, button_outside_menu(&mask), xTaskGetTickCount());
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
            // Pick up whatever was just saved. Deliberately not re-seeding
            // the settings menu from it: provisioning writes the network and
            // the server URI and never the three settings, so there is
            // nothing new to take, and settings_init() is a construction
            // call - it zeroes the gesture state along with the values, which
            // is not a thing to do to a live menu.
            config_load(&cfg);
        } else {
            // Nothing else to try. Fall through and attempt whatever is
            // configured: with no credentials that waits forever, which is at
            // least visible on the panel rather than a silent reboot loop.
            ESP_LOGE(TAG, "provisioning would not start");
        }
    }

    // See boot_gesture_task: net_task does not exist yet to catch the reset
    // gesture, and wifi_connect() waits with no timeout, so this is the only
    // window where the button would otherwise be undetectable.
    TaskHandle_t boot_gesture = NULL;
    xTaskCreate(boot_gesture_task, "boot_gesture", 2048, NULL, 4, &boot_gesture);
    wifi_connect(cfg.ssid, cfg.pass);
    vTaskDelete(boot_gesture);

    ws_start(cfg.uri);

    xTaskCreate(audio_in_task, "audio_in", 4096, NULL, 5, NULL);
    xTaskCreate(audio_out_task, "audio_out", 4096, NULL, 5, NULL);
    xTaskCreate(net_task, "net", 4096, NULL, 4, NULL);
    xTaskCreate(led_task, "led", 2048, NULL, 2, NULL);
    xTaskCreate(link_task, "link", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "ready - hold the button on GPIO%d and speak%s", PIN_BUTTON,
             s_have_panel ? "" : " (no display)");
}
