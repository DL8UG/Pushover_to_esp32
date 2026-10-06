#pragma once

// Repeating alarm over audio.h's audio_tone() on the board's ES8311 codec
// and speaker (there is no separate buzzer).
void buzzer_init(void);

// Starts or stops the repeating alarm: while active, plays series of 2-6
// beeps with a pause in between, on a loop - pitch, length, gaps and
// series length all random, so it doesn't fade into background noise the
// way a perfectly regular tone can. Driven purely by whether any undeleted high-priority
// message remains - pushover_client.cpp calls this with
// (message count > 0) after every store change, so the alarm runs for as
// long as anything is unread and stops the moment the count reaches 0.
// Safe to call repeatedly with the same value.
void buzzer_set_alarm(bool active);

// Global mute toggle for the alarm, independent of buzzer_set_alarm()'s
// "is there anything to alarm about" state - toggled by a long press on
// both buttons together (see buttons.cpp). Defaults to enabled at boot.
// Mirrors itself onto the home screen's bell icon (display.h).
void buzzer_toggle_notifications(void);
bool buzzer_notifications_enabled(void);

// Starts or stops a separate, quiet "can't reach Pushover" warning beep -
// a short, quiet tone repeated every few seconds, independent of (and
// able to run alongside) the louder message alarm above, since the two
// signal different things: an undelivered message vs. the device itself
// being unable to find out about new ones at all. Driven by
// pushover_client.cpp's connection watchdog once the WS link has been
// down for 15 continuous minutes; NOT affected by
// buzzer_toggle_notifications()'s mute, matching the home screen's
// "NOT CONNECTED" warning also ignoring mute (display.h) - this is a
// device-health signal, not a per-message alert.
void buzzer_set_disconnected_warning(bool active);
