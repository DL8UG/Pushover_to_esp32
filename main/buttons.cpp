#include "buttons.h"
#include "button_input.h"
#include "pushover_client.h"
#include "buzzer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

void poll_task(void *)
{
    ButtonEvent ev;
    for (;;) {
        while (button_input_receive(ev)) {
            if (ev.gesture == GESTURE_LONG) {
                if (ev.channel == BTN_BOTH) {
                    buzzer_toggle_notifications();
                } else {
                    pushover_ack_current();
                }
            } else if (ev.gesture == GESTURE_SINGLE) {
                if (ev.channel == BTN_BOOT_ONLY) {
                    pushover_show_prev();
                } else if (ev.channel == BTN_PWR_ONLY) {
                    pushover_show_next();
                }
            }
            // GESTURE_DOUBLE: no action mapped.
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

} // namespace

void buttons_init(void)
{
    button_input_start();
    // Not just a poller: navigation renders and refreshes the display on
    // this task (pushover_show_next/prev -> display_show_message), which
    // overflowed 3072 bytes and reset the device.
    xTaskCreate(poll_task, "buttons_poll", 6144, nullptr, 5, nullptr);
}
