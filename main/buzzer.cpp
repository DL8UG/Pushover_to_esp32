#include "buzzer.h"
#include "audio.h"
#include "display.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"

namespace {

volatile bool s_alarm_active = false;
volatile bool s_notifications_enabled = true;

// 1.5-3 kHz: within the band of peak human hearing sensitivity (equal-
// loudness contours bottom out roughly there) and the range typical
// smoke/CO alarms use because it stays clearly audible to a sleeping
// person without being so high it sounds like electronic noise - neither
// too low nor too high for waking someone from deep sleep.
constexpr float FREQ_MIN = 1500.0f;
constexpr float FREQ_MAX = 3000.0f;
// Short: with a new random pitch every beep (see alarm_task), a short
// duration is what makes the pitch actually flicker fast instead of just
// holding one random note for a whole second.
constexpr float DURATION_MIN_S = 0.08f;
constexpr float DURATION_MAX_S = 0.35f;
constexpr int BEEPS_MIN = 2;
constexpr int BEEPS_MAX = 6;
constexpr int BEEP_GAP_MIN_MS = 40;    // between beeps within a series
constexpr int BEEP_GAP_MAX_MS = 350;
constexpr int SERIES_GAP_MIN_MS = 100; // pause after a series, before the next
constexpr int SERIES_GAP_MAX_MS = 900;

float random_range(float lo, float hi)
{
    float frac = (float)esp_random() / (float)UINT32_MAX;
    return lo + frac * (hi - lo);
}

int random_int(int lo, int hi) // inclusive
{
    return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1));
}

bool should_sound(void) { return s_alarm_active && s_notifications_enabled; }

// Idles while inactive or muted; otherwise loops a series of beeps, each
// with its own random pitch AND length - deliberately as unpleasant and
// unrhythmic as possible: beep count per series, the gap between beeps
// and the pause between series are all re-rolled every time too, so
// there is no steady beat or pitch to tune out. Runs for the whole program lifetime -
// simpler and avoids task-create/destroy churn on every message that
// arrives or gets deleted, which can happen in bursts.
void alarm_task(void *)
{
    for (;;) {
        if (!should_sound()) {
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }
        int beeps = random_int(BEEPS_MIN, BEEPS_MAX);

        for (int i = 0; i < beeps && should_sound(); i++) {
            audio_tone(random_range(FREQ_MIN, FREQ_MAX), random_range(DURATION_MIN_S, DURATION_MAX_S));
            if (i < beeps - 1 && should_sound()) {
                vTaskDelay(pdMS_TO_TICKS(random_int(BEEP_GAP_MIN_MS, BEEP_GAP_MAX_MS)));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(random_int(SERIES_GAP_MIN_MS, SERIES_GAP_MAX_MS)));
    }
}

volatile bool s_disconnected_warning_active = false;

constexpr float DISCONNECTED_FREQ = 900.0f;
constexpr float DISCONNECTED_DURATION_S = 0.15f;
constexpr float DISCONNECTED_AMPLITUDE = 6000.0f; // quiet - well under the alarm's 28000
constexpr int DISCONNECTED_INTERVAL_MS = 4000;

// A separate loop from alarm_task above - deliberately simple (fixed
// pitch/length/interval, not randomized) so it reads as a distinct,
// steady "something's wrong with connectivity" cue rather than being
// confused with the louder, randomized message alarm. Can run at the
// same time as that one; they're independent signals.
void disconnected_task(void *)
{
    for (;;) {
        if (s_disconnected_warning_active) {
            audio_tone(DISCONNECTED_FREQ, DISCONNECTED_DURATION_S, DISCONNECTED_AMPLITUDE);
            vTaskDelay(pdMS_TO_TICKS(DISCONNECTED_INTERVAL_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(150));
        }
    }
}

} // namespace

void buzzer_init(void)
{
    audio_init();
    display_set_notifications_enabled(s_notifications_enabled);
    xTaskCreate(alarm_task, "buzzer_alarm", 3072, nullptr, 4, nullptr);
    xTaskCreate(disconnected_task, "buzzer_disc", 3072, nullptr, 4, nullptr);
}

void buzzer_set_alarm(bool active)
{
    s_alarm_active = active;
}

void buzzer_toggle_notifications(void)
{
    s_notifications_enabled = !s_notifications_enabled;
    display_set_notifications_enabled(s_notifications_enabled);
}

bool buzzer_notifications_enabled(void)
{
    return s_notifications_enabled;
}

void buzzer_set_disconnected_warning(bool active)
{
    s_disconnected_warning_active = active;
}
