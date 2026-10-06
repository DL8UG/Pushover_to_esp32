#include "button_input.h"
#include "user_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

namespace {

const char *TAG = "button";

constexpr int POLL_INTERVAL_MS = 20;
// Time-based debounce: a raw reading only becomes the new stable state
// after it's held continuously for this long, so any noise/chatter
// (electrical or mechanical) that flips mid-window just keeps resetting
// the timer and never commits - see debounce_update(). Raised from 60ms
// after a burst of rapid single-clicks was observed on this board's PWR
// button (GPIO18, no external pull-up on this board - only the internal
// weak one) right after boot; 80ms is still well under human perception
// for a deliberate press.
constexpr int64_t DEBOUNCE_MS = 80;
constexpr int64_t LONG_PRESS_MS = 700;
constexpr int64_t DOUBLE_CLICK_GAP_MS = 350;
// Two fingers essentially never land in the same 20ms poll tick. Give a
// single-button press this long to see if the other button joins in before
// committing to it as an individual gesture -- otherwise every "both"
// attempt fires an extra solo click for whichever button arrived first.
constexpr int64_t COMBO_GRACE_MS = 60;

QueueHandle_t s_queue = nullptr;

struct DebouncedPin {
    gpio_num_t pin;
    bool stable_state = false; // debounced "is pressed" (active low)
    bool last_raw = false;
    int64_t last_change_ms = 0;
};

bool debounce_update(DebouncedPin &d, int64_t now_ms) {
    bool raw_pressed = gpio_get_level(d.pin) == 0;
    if (raw_pressed != d.last_raw) {
        // Debug-only: lets a future serial capture tell genuine chatter
        // (many raw flips in a tight burst) apart from real presses,
        // without spamming the normal INFO-level "Gesture:" log.
        ESP_LOGD(TAG, "pin %d raw -> %d at %lldms", (int)d.pin, (int)raw_pressed, (long long)now_ms);
        d.last_raw = raw_pressed;
        d.last_change_ms = now_ms;
    }
    if (raw_pressed != d.stable_state && (now_ms - d.last_change_ms) >= DEBOUNCE_MS) {
        d.stable_state = raw_pressed;
    }
    return d.stable_state;
}

const char *channel_name(ButtonChannel c) {
    switch (c) {
        case BTN_BOOT_ONLY: return "BOOT";
        case BTN_PWR_ONLY:  return "PWR";
        default:            return "BOTH";
    }
}

const char *gesture_name(ButtonGesture g) {
    switch (g) {
        case GESTURE_SINGLE: return "SINGLE";
        case GESTURE_DOUBLE: return "DOUBLE";
        default:             return "LONG";
    }
}

void emit(ButtonChannel channel, ButtonGesture gesture) {
    ESP_LOGI(TAG, "Gesture: %s %s", channel_name(channel), gesture_name(gesture));
    ButtonEvent ev{channel, gesture};
    xQueueSend(s_queue, &ev, 0);
}

void button_task(void *arg) {
    DebouncedPin boot_pin{(gpio_num_t)BOOT_BUTTON_PIN};
    DebouncedPin pwr_pin{(gpio_num_t)PWR_BUTTON_PIN};

    enum State { IDLE, PENDING_CLASSIFY, PRESSED, RELEASED_WAITING_DOUBLE, PRESSED2 };
    State state = IDLE;
    ButtonChannel channel = BTN_BOOT_ONLY; // meaningless until the first press
    int64_t press_start_ms = 0, pending_since = 0;
    bool long_fired = false;

    while (true) {
        int64_t now = esp_timer_get_time() / 1000;

        bool boot_down = debounce_update(boot_pin, now);
        bool pwr_down = debounce_update(pwr_pin, now);

        bool any_down = boot_down || pwr_down;
        ButtonChannel raw_mode = boot_down && pwr_down ? BTN_BOTH
                                : boot_down             ? BTN_BOOT_ONLY
                                                         : BTN_PWR_ONLY;

        switch (state) {
        case IDLE:
            if (raw_mode == BTN_BOTH) {
                // both landed in the same poll tick -- unambiguous, no need
                // to wait out the grace window.
                state = PRESSED;
                channel = BTN_BOTH;
                press_start_ms = now;
                long_fired = false;
            } else if (any_down) {
                state = PENDING_CLASSIFY;
                channel = raw_mode; // tentative
                press_start_ms = now;
            }
            break;

        case PENDING_CLASSIFY:
            if (raw_mode == BTN_BOTH) {
                // the other button joined within the grace window -- this
                // was a combo attempt all along. Keep the original
                // press_start_ms so long-press timing isn't skewed by the
                // grace period.
                state = PRESSED;
                channel = BTN_BOTH;
                long_fired = false;
            } else if (!any_down) {
                // released before the other button ever joined -- resolve as
                // a genuine short press-release of the original button.
                if (now - press_start_ms < DEBOUNCE_MS) {
                    state = IDLE; // too short to be real, drop it
                } else {
                    pending_since = now;
                    state = RELEASED_WAITING_DOUBLE;
                }
            } else if (raw_mode == channel) {
                if (now - press_start_ms >= COMBO_GRACE_MS) {
                    // grace window expired, no combo -- commit to this button.
                    state = PRESSED;
                    long_fired = false;
                }
            } else {
                // switched straight to the other single button without a
                // both-down tick in between (rare) -- restart classification.
                channel = raw_mode;
                press_start_ms = now;
            }
            break;

        case PRESSED:
            if (any_down && raw_mode == channel) {
                if (!long_fired && (now - press_start_ms) >= LONG_PRESS_MS) {
                    emit(channel, GESTURE_LONG);
                    long_fired = true;
                }
            } else if (!any_down) {
                // genuine release
                if (long_fired) {
                    state = IDLE;
                } else if (now - press_start_ms < DEBOUNCE_MS) {
                    state = IDLE; // too short to be real, drop it
                } else {
                    pending_since = now;
                    state = RELEASED_WAITING_DOUBLE;
                }
            } else {
                // the held combination changed (other button joined/replaced this
                // one) before release or a long-press fired -- discard silently,
                // start fresh for the new combination.
                state = PRESSED;
                channel = raw_mode;
                press_start_ms = now;
                long_fired = false;
            }
            break;

        case RELEASED_WAITING_DOUBLE:
            if (any_down && raw_mode == channel && (now - pending_since) <= DOUBLE_CLICK_GAP_MS) {
                state = PRESSED2;
                press_start_ms = now;
            } else if (any_down) {
                // a different combination started before the gap timed out --
                // resolve the pending click as a single, then start fresh.
                emit(channel, GESTURE_SINGLE);
                channel = raw_mode;
                state = PRESSED;
                press_start_ms = now;
                long_fired = false;
            } else if (now - pending_since > DOUBLE_CLICK_GAP_MS) {
                emit(channel, GESTURE_SINGLE);
                state = IDLE;
            }
            break;

        case PRESSED2:
            if (any_down && raw_mode == channel) {
                // A brief mid-hold debounce blip can land here (release
                // detected, then the still-physically-held button reappears
                // within the double-click gap, misread as a second press).
                // Keep checking for a long-press so that case still resolves
                // correctly instead of only ever being able to emit DOUBLE.
                if (!long_fired && (now - press_start_ms) >= LONG_PRESS_MS) {
                    emit(channel, GESTURE_LONG);
                    long_fired = true;
                }
            } else if (!any_down) {
                if (long_fired) {
                    state = IDLE; // was actually one continuous long hold, not a double
                } else {
                    emit(channel, GESTURE_DOUBLE);
                    state = IDLE;
                }
            } else {
                // reclassified mid-second-press -- discard entirely, start fresh
                state = PRESSED;
                channel = raw_mode;
                press_start_ms = now;
                long_fired = false;
            }
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

} // namespace

void button_input_start(void) {
    gpio_config_t btn_cfg = {};
    btn_cfg.intr_type = GPIO_INTR_DISABLE;
    btn_cfg.mode = GPIO_MODE_INPUT;
    btn_cfg.pin_bit_mask = (1ULL << BOOT_BUTTON_PIN) | (1ULL << PWR_BUTTON_PIN);
    btn_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&btn_cfg);

    s_queue = xQueueCreate(16, sizeof(ButtonEvent));
    xTaskCreate(button_task, "button_input", 4096, nullptr, 5, nullptr);
}

bool button_input_receive(ButtonEvent &out) {
    return xQueueReceive(s_queue, &out, 0) == pdTRUE;
}
