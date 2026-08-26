// Sixty-second question: does this board have an LED on GPIO 8, and which
// level lights it?
//
// Most ESP32-C3 devkits put one there and wire it active-low, but "most" is
// not "this one", and the state indicator is not worth building on a guess.
//
// The pattern is deliberately asymmetric so polarity can be read off it
// without a meter:
//
//     3 short pulses at level HIGH, one second dark, then
//     3 short pulses at level LOW,  two seconds dark, repeat
//
// Whichever group is visible tells us which level turns the LED on. If nothing
// is visible at all, the board has no LED on this pin.
//
// GPIO 8 is a strapping pin. It is only sampled at reset, so driving it
// afterwards is safe - but this sketch leaves it HIGH between cycles, which is
// the level a normal boot wants.

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_LED GPIO_NUM_8

static const char *TAG = "led";

void app_main(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_LED,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    ESP_LOGI(TAG, "GPIO%d held HIGH for 1 s, then LOW for 3 s, repeating",
             PIN_LED);

    // Static levels, not pulses. The first version of this toggled the pin
    // inside each group, which looks identical whichever level lights the LED
    // and so answered nothing. Holding a level and timing it does answer:
    // whichever level is on for three quarters of the cycle is the lit one.
    while (true) {
        printf("pin HIGH (1 s)\n");
        gpio_set_level(PIN_LED, 1);
        vTaskDelay(pdMS_TO_TICKS(1000));

        printf("pin LOW  (3 s)\n");
        gpio_set_level(PIN_LED, 0);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
