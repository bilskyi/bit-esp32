// Safety sketch: hold the amplifier shut down and do nothing else.
//
// Flash this when the board is making noise you want stopped. It drives the
// MAX98357A shutdown pin low and idles. I2S is never initialised, so BCLK, WS
// and DIN stay quiet too.

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_MUTE GPIO_NUM_10

void app_main(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_MUTE,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_set_level(PIN_MUTE, 0));

    while (true) {
        gpio_set_level(PIN_MUTE, 0);  // re-assert, in case anything glitches it
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
