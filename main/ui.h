#pragma once
#include <cstdint>

#include "render.h"

// Everything the screen shows, as plain data. display.cpp keeps one of
// these up to date and redraws from it; the PC simulator (test/sim) fills
// it with test scenarios. No hardware or RTOS code in here.
struct UiState {
    enum class Screen { Home, Message };
    Screen screen = Screen::Home;

    // Current time (Unix, UTC), shown in local time (TZ) in the home
    // screen's status bar; 0 = clock not set yet ("--:--").
    int64_t now = 0;

    // Home screen
    bool wifi_ok = false;
    bool pushover_ok = false;
    bool alarm_enabled = true;
    bool unreachable = false; // Pushover down for a long time: warning banner
    int count = 0;            // stored (unacknowledged) messages

    // Message screen
    char title[160] = "";
    int64_t date = 0; // Unix time, 0 = unknown; shown in local time (TZ)
    int priority = 1; // Pushover scale, 2 = emergency
    int index = 0;    // 0-based position of this message ...
    int total = 0;    // ... among this many
};

// True once `unix_time` looks like a real clock (SNTP has synced), not the
// 1970-based count the clock starts with after a reset.
bool clock_is_set(int64_t unix_time);

// Draws the complete screen for `s` into `c` (cleared first).
void ui_render(const UiState &s, Canvas &c);
