#include "display.h"

#include <cstdio>
#include <ctime>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "user_config.h"
#include "ssd1681.h"
#include "ui.h"

namespace {

const char *TAG = "display";

Ssd1681 *s_epd = nullptr;

// Every display_* entry point below is called from a different task -
// pushover_client.cpp's connection watchdog, its WS event handler (runs
// on the WS client's own task), its fetch/ack tasks, buttons.cpp's poll
// task, and the timeout callback (runs on esp_timer's own service task).
// Without this, two of them racing could interleave SPI transactions
// mid-frame - the SPI driver itself serializes individual transactions,
// but a whole frame update is not atomic on its own.
SemaphoreHandle_t s_mutex = nullptr;

UiState s_ui;
Canvas s_canvas;
uint8_t s_frame[Ssd1681::FRAME_BYTES]; // controller format: bit set = white

// Falls back from the message view to the home screen after this long with
// no further next/prev/ack - the device should always settle back to the
// at-a-glance home screen on its own, not stay stuck on one message.
constexpr int64_t MESSAGE_TIMEOUT_US = 30 * 1000000LL;
esp_timer_handle_t s_timeout_timer = nullptr;

// Wakes up at every full minute for the clock in the status bar.
esp_timer_handle_t s_clock_timer = nullptr;

int64_t s_last_full_refresh_ms = 0;
// Periodic ghosting cleanup. A full refresh flashes the whole panel, so
// not more often than this - with the clock the screen changes every
// minute, and partial refreshes in between are fine.
constexpr int64_t FULL_REFRESH_INTERVAL_MS = 60 * 60 * 1000;

// Renders the whole screen from s_ui and pushes it. Partial refresh
// normally (fast, low-flicker); a full refresh instead every
// FULL_REFRESH_INTERVAL_MS to clear e-paper ghosting.
void redraw(void)
{
    if (!s_epd) return;
    s_ui.now = time(nullptr);
    ui_render(s_ui, s_canvas);

    // Same layout as the controller RAM, only with inverted colour.
    static_assert(Canvas::BYTES == Ssd1681::FRAME_BYTES, "canvas and panel differ");
    const uint8_t *bits = s_canvas.bits();
    for (int i = 0; i < Ssd1681::FRAME_BYTES; i++) s_frame[i] = ~bits[i];

    int64_t now = esp_timer_get_time() / 1000;
    if (now - s_last_full_refresh_ms >= FULL_REFRESH_INTERVAL_MS) {
        s_last_full_refresh_ms = now;
        s_epd->full_refresh(s_frame);
    } else {
        s_epd->partial_refresh(s_frame);
    }
}

bool home_showing(void)
{
    return s_ui.screen == UiState::Screen::Home;
}

// Re-arms the clock timer for just after the next full minute.
void schedule_clock_tick(void)
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    int64_t us_into_minute = (int64_t)(tv.tv_sec % 60) * 1000000 + tv.tv_usec;
    esp_timer_start_once(s_clock_timer, 60 * 1000000LL - us_into_minute + 200000);
}

void clock_tick_cb(void *)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (home_showing()) redraw();
    xSemaphoreGive(s_mutex);
    schedule_clock_tick();
}

void timeout_cb(void *)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_ui.screen = UiState::Screen::Home;
    redraw();
    xSemaphoreGive(s_mutex);
}

// Sets one home-screen field and redraws if it changed while visible.
template <typename T>
void set_home_field(T UiState::*field, T value)
{
    if (!s_epd) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_ui.*field != value) {
        s_ui.*field = value;
        if (home_showing()) redraw();
    }
    xSemaphoreGive(s_mutex);
}

} // namespace

void display_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    Ssd1681::Pins pins = {};
    pins.cs = EPD_CS_PIN;
    pins.dc = EPD_DC_PIN;
    pins.rst = EPD_RST_PIN;
    pins.busy = EPD_BUSY_PIN;
    pins.mosi = EPD_MOSI_PIN;
    pins.sck = EPD_SCK_PIN;
    pins.host = EPD_SPI_NUM;

    s_epd = new Ssd1681(pins);
    s_epd->begin();
    // The first redraw() below is a full refresh: wipes whatever the panel
    // showed before the reset and sets the reference for partial updates.
    s_last_full_refresh_ms = -FULL_REFRESH_INTERVAL_MS;

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = &timeout_cb;
    timer_args.name = "display_timeout";
    esp_timer_create(&timer_args, &s_timeout_timer);

    esp_timer_create_args_t clock_args = {};
    clock_args.callback = &clock_tick_cb;
    clock_args.name = "display_clock";
    esp_timer_create(&clock_args, &s_clock_timer);
    schedule_clock_tick();

    redraw();
    ESP_LOGI(TAG, "display ready (%dx%d e-paper)", Ssd1681::WIDTH, Ssd1681::HEIGHT);
}

void display_clock_changed(void)
{
    // Only fires the clock timer right away: callers like the SNTP sync
    // callback run on the network stack's task, which must not wait for
    // an e-paper refresh. The tick redraws and realigns to full minutes.
    if (!s_clock_timer) return;
    esp_timer_stop(s_clock_timer); // no-op if not running
    esp_timer_start_once(s_clock_timer, 1000);
}

void display_set_wifi_status(bool ok)
{
    set_home_field(&UiState::wifi_ok, ok);
}

void display_set_pushover_status(bool ok)
{
    set_home_field(&UiState::pushover_ok, ok);
}

void display_set_message_count(int count)
{
    set_home_field(&UiState::count, count);
}

void display_set_notifications_enabled(bool enabled)
{
    set_home_field(&UiState::alarm_enabled, enabled);
}

void display_set_pushover_unreachable(bool unreachable)
{
    set_home_field(&UiState::unreachable, unreachable);
}

void display_show_idle(void)
{
    if (!s_epd) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_ui.screen = UiState::Screen::Home;
    if (s_timeout_timer) esp_timer_stop(s_timeout_timer); // no-op if not running
    redraw();
    xSemaphoreGive(s_mutex);
}

void display_show_message(const char *title, int64_t date, int priority, int index, int count)
{
    if (!s_epd) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_ui.title, sizeof(s_ui.title), "%s", title ? title : "");
    s_ui.date = date;
    s_ui.priority = priority;
    s_ui.index = index;
    s_ui.total = count;
    s_ui.count = count; // keep the home count in sync for when the timeout falls back to it
    s_ui.screen = UiState::Screen::Message;

    if (s_timeout_timer) {
        esp_timer_stop(s_timeout_timer); // no-op if not running
        esp_timer_start_once(s_timeout_timer, MESSAGE_TIMEOUT_US);
    }
    redraw();
    xSemaphoreGive(s_mutex);
}
