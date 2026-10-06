#pragma once
#include <cstdint>

// Waveshare ESP32-S3-ePaper-1.54 (200x200, SPI, partial-refresh capable).
// The screens themselves are drawn by ui.cpp into a framebuffer; this
// module keeps the UiState, ships finished frames to the panel and handles
// partial/full refresh. Must be called once, after the EPD power rail is
// on (board_power_init(), done in main.cpp) and before any of the other
// display_* calls below. All display_* calls are safe from any task.
void display_init(void);

// The home screen's status bar also shows the current time (local time
// zone, see LOCAL_TIMEZONE), updated every minute by display.cpp itself.
// Call this after the clock was set (SNTP) to show it right away and
// realign the minute updates; safe from any task, returns immediately.
void display_clock_changed(void);

// Home screen: WiFi / Pushover status icons in the status bar. Redraws
// immediately if the value changed and the home screen is showing;
// otherwise just updates the state for next time.
void display_set_wifi_status(bool ok);
void display_set_pushover_status(bool ok);

// Home screen: number of stored messages (big number, or "All clear").
void display_set_message_count(int count);

// Home screen: bell icon in the status bar, crossed out while muted.
void display_set_notifications_enabled(bool enabled);

// Home screen: "NOT CONNECTED" banner while Pushover has been unreachable
// long enough to warn about (pushover_client.cpp's watchdog). Shown
// regardless of the mute state - it is a device-health signal, not a
// per-message one.
void display_set_pushover_unreachable(bool unreachable);

// Switches to the home screen right now and cancels any pending
// message-view timeout.
void display_show_idle(void);

// Switches to the message view: title of message (index+1) of `count`,
// with its date (Unix time, 0 = unknown) and priority in the header.
// Returns to the home screen after 30 s without a further call, so the
// device always settles back to the at-a-glance screen on its own.
void display_show_message(const char *title, int64_t date, int priority, int index, int count);
