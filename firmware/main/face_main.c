// Bring-up sketch for the OLED, in the same spirit as capture and playback:
// prove the display and its wiring here, on a serial console, rather than
// three layers later inside a WebSocket session.
//
//   idf.py -DSKETCH=face -p /dev/cu.usbmodem1101 flash monitor
//
// No WiFi, no server, no audio. It tours every state and every emotion, and
// the button interrupts the tour with a whole fake conversation - listening
// while held, then thinking, then speaking - so the interactive parts can be
// checked too.
//
// It also reports how long a frame actually takes to reach the panel, because
// the 23 ms predicted for a full 1 KB frame at 400 kHz is arithmetic, and this
// project has a long history of arithmetic that turned out to be beside the
// point.

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "face.h"
#include "ssd1306.h"

#define PIN_SDA GPIO_NUM_0
#define PIN_SCL GPIO_NUM_1
#define PIN_BUTTON GPIO_NUM_3
#define PIN_MUTE GPIO_NUM_10
#define PIN_LED GPIO_NUM_8
#define LED_ON 0
#define LED_OFF 1

#define I2C_HZ 400000
#define FRAME_MS 40  // 25 fps

static const char *TAG = "face";

static ssd1306_t s_panel;
static face_t s_face;

// ------------------------------------------------------------------ the tour

typedef struct {
    const char *label;
    face_state_t state;
    face_emotion_t emotion;
    bool energy;
    uint32_t ms;
} step_t;

static const step_t TOUR[] = {
    {"idle, neutral", FACE_ST_IDLE, FACE_EMO_NEUTRAL, false, 6000},
    {"listening", FACE_ST_LISTENING, FACE_EMO_NEUTRAL, true, 4000},
    {"thinking", FACE_ST_THINKING, FACE_EMO_NEUTRAL, false, 4000},
    {"speaking, neutral", FACE_ST_SPEAKING, FACE_EMO_NEUTRAL, true, 3500},
    {"speaking, happy", FACE_ST_SPEAKING, FACE_EMO_HAPPY, true, 3500},
    {"speaking, excited", FACE_ST_SPEAKING, FACE_EMO_EXCITED, true, 3500},
    {"speaking, curious", FACE_ST_SPEAKING, FACE_EMO_CURIOUS, true, 3500},
    {"speaking, confused", FACE_ST_SPEAKING, FACE_EMO_CONFUSED, true, 3500},
    {"speaking, surprised", FACE_ST_SPEAKING, FACE_EMO_SURPRISED, true, 3500},
    {"speaking, sad", FACE_ST_SPEAKING, FACE_EMO_SAD, true, 3500},
    {"speaking, annoyed", FACE_ST_SPEAKING, FACE_EMO_ANNOYED, true, 3500},
    {"speaking, sleepy", FACE_ST_SPEAKING, FACE_EMO_SLEEPY, true, 3500},
    {"offline, asleep", FACE_ST_OFFLINE, FACE_EMO_NEUTRAL, false, 5000},
};

#define TOUR_STEPS (sizeof(TOUR) / sizeof(TOUR[0]))

// A syllabic envelope standing in for real speech: phrases of about 1.7 s with
// a breath between them, syllables at roughly 4.5 Hz.
static uint16_t synth_rms(uint32_t t) {
    if ((t % 2100u) > 1700u) return 0;
    const uint32_t syl = t % 222u;
    const int32_t tri = (int32_t)(syl < 111u ? syl : 222u - syl) * 255 / 111;
    static const int32_t stress[7] = {255, 178, 232, 120, 255, 158, 205};
    return (uint16_t)(9000 * (tri * stress[(t / 222u) % 7u] / 255) / 255);
}

static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// ------------------------------------------------------------------- app

void app_main(void) {
    // The amplifier shares this board and its shutdown pin floats until
    // something drives it. Hold it low so a display sketch cannot make noise.
    const gpio_config_t mute = {
        .pin_bit_mask = 1ULL << PIN_MUTE,
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&mute));
    gpio_set_level(PIN_MUTE, 0);

    const gpio_config_t btn = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));

    const gpio_config_t led = {
        .pin_bit_mask = 1ULL << PIN_LED,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&led));
    gpio_set_level(PIN_LED, LED_OFF);

    ESP_LOGI(TAG, "SDA on GPIO%d, SCL on GPIO%d, button on GPIO%d",
             (int)PIN_SDA, (int)PIN_SCL, (int)PIN_BUTTON);

    const esp_err_t err = ssd1306_init(&s_panel, PIN_SDA, PIN_SCL, I2C_HZ);
    if (err != ESP_OK) {
        // Say what to check, rather than leaving a bare error code. The three
        // causes below are essentially the whole list.
        ESP_LOGE(TAG, "no panel: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "  - SDA to GPIO%d and SCL to GPIO%d, not swapped",
                 (int)PIN_SDA, (int)PIN_SCL);
        ESP_LOGE(TAG, "  - the module needs 3V3 and ground");
        ESP_LOGE(TAG, "  - some modules are strapped to 0x3D; both were tried");
        while (true) {
            // A slow heartbeat, so a dead sketch is distinguishable from a
            // dead board.
            gpio_set_level(PIN_LED, LED_ON);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(PIN_LED, LED_OFF);
            vTaskDelay(pdMS_TO_TICKS(900));
        }
    }

    face_init(&s_face, now_ms());

    size_t step = 0;
    uint32_t step_started = now_ms();
    bool announced = false;
    bool button_was_down = false;

    // The fake conversation the button drives, and where it is up to.
    enum { MODE_TOUR, MODE_HELD, MODE_THINKING, MODE_SPEAKING } mode = MODE_TOUR;
    uint32_t mode_started = 0;
    face_emotion_t reply_emotion = FACE_EMO_HAPPY;

    uint32_t flush_min = UINT32_MAX, flush_max = 0, flush_sum = 0, flushes = 0;
    uint32_t reported = now_ms();

    while (true) {
        const uint32_t t = now_ms();
        const bool down = gpio_get_level(PIN_BUTTON) == 0;

        if (down != button_was_down) {
            button_was_down = down;
            face_set_button(&s_face, down, t);
            if (down) {
                mode = MODE_HELD;
                mode_started = t;
                face_set_state(&s_face, FACE_ST_LISTENING, t);
                ESP_LOGI(TAG, "button down: listening");
            } else if (mode == MODE_HELD) {
                mode = MODE_THINKING;
                mode_started = t;
                face_set_state(&s_face, FACE_ST_THINKING, t);
                ESP_LOGI(TAG, "button up: thinking");
            }
        }

        switch (mode) {
            case MODE_HELD:
                face_feed_energy(&s_face, synth_rms(t - mode_started));
                break;

            case MODE_THINKING:
                if (t - mode_started > 1200u) {
                    mode = MODE_SPEAKING;
                    mode_started = t;
                    // Rotate through the emotions so repeated presses show
                    // different ones.
                    reply_emotion = (face_emotion_t)((reply_emotion + 1) % FACE_EMO_COUNT);
                    face_set_emotion(&s_face, reply_emotion, t);
                    face_set_state(&s_face, FACE_ST_SPEAKING, t);
                    ESP_LOGI(TAG, "speaking, %s", face_emotion_name(reply_emotion));
                }
                break;

            case MODE_SPEAKING:
                face_feed_energy(&s_face, synth_rms(t - mode_started));
                if (t - mode_started > 3500u) {
                    mode = MODE_TOUR;
                    step_started = t;
                    announced = false;
                    face_set_state(&s_face, FACE_ST_IDLE, t);
                    ESP_LOGI(TAG, "back to the tour");
                }
                break;

            case MODE_TOUR:
            default: {
                const step_t *s = &TOUR[step];
                if (!announced) {
                    face_set_emotion(&s_face, s->emotion, t);
                    face_set_state(&s_face, s->state, t);
                    ESP_LOGI(TAG, "%u/%u  %s", (unsigned)step + 1,
                             (unsigned)TOUR_STEPS, s->label);
                    announced = true;
                }
                if (s->energy) face_feed_energy(&s_face, synth_rms(t - step_started));
                if (t - step_started > s->ms) {
                    step = (step + 1) % TOUR_STEPS;
                    step_started = t;
                    announced = false;
                }
                break;
            }
        }

        face_tick(&s_face, t);

        const int64_t before = esp_timer_get_time();
        const esp_err_t ferr = ssd1306_flush(&s_panel, face_framebuffer(&s_face));
        const uint32_t took = (uint32_t)(esp_timer_get_time() - before);

        if (ferr != ESP_OK) {
            ESP_LOGW(TAG, "flush failed: %s", esp_err_to_name(ferr));
        } else if (took > 0) {
            if (took < flush_min) flush_min = took;
            if (took > flush_max) flush_max = took;
            flush_sum += took;
            flushes++;
        }

        // The number that decides whether 25 fps fits in the frame budget, and
        // whether the I2C clock is worth raising past the datasheet's 400 kHz.
        if (t - reported > 10000u && flushes > 0) {
            ESP_LOGI(TAG, "flush %lu/%lu/%lu us over %lu frames, %lu us budget",
                     (unsigned long)flush_min, (unsigned long)(flush_sum / flushes),
                     (unsigned long)flush_max, (unsigned long)flushes,
                     (unsigned long)(FRAME_MS * 1000));
            flush_min = UINT32_MAX;
            flush_max = flush_sum = flushes = 0;
            reported = t;
        }

        gpio_set_level(PIN_LED, down ? LED_ON : LED_OFF);

        // Pace on the clock rather than sleeping a fixed amount, so a slow
        // flush eats into the idle time instead of stretching the frame.
        const uint32_t spent = now_ms() - t;
        vTaskDelay(pdMS_TO_TICKS(spent >= FRAME_MS ? 1 : FRAME_MS - spent));
    }
}
