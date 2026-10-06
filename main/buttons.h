#pragma once

// The e-Paper board's two physical buttons (BOOT = GPIO0, PWR = GPIO18),
// read via button_input.h's debounced gesture detector. Short press on
// either scrolls between stored Pushover messages (BOOT = previous,
// PWR = next); a long press on either acks/clears the current message
// (and everything older). A long press on BOTH together toggles
// the alarm on/off (buzzer.h), independent of message navigation.
void buttons_init(void);
