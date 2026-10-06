#include "board_power.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "user_config.h"

void board_power_init(void)
{
    gpio_config_t cfg = {};
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pin_bit_mask = (1ULL << EPD_PWR_PIN) | (1ULL << AUDIO_PWR_PIN) | (1ULL << VBAT_PWR_PIN);
    ESP_ERROR_CHECK(gpio_config(&cfg));

    gpio_set_level(EPD_PWR_PIN, 0);   // on
    gpio_set_level(AUDIO_PWR_PIN, 0); // on
    // Battery latch left low, as the firmware always had it - the device
    // runs on USB power.
    gpio_set_level(VBAT_PWR_PIN, 0);

    vTaskDelay(pdMS_TO_TICKS(10)); // let the rails settle
}
