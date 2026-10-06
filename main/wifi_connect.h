#pragma once

// Multi-profile WiFi STA connect + reconnect. Up to 3 SSID/password pairs
// come from secrets.h (compiled in - this board has no touch/keyboard for
// runtime entry). On a connection failure, cycles round-robin through the
// configured profiles rather than only ever retrying the one that just
// failed - see wifi_connect.cpp's event_handler(). No NVS profile store
// or scan UI: with two buttons there is no way to enter credentials on
// the device anyway.

void wifi_connect_start(void);

// Blocks the calling task until the first successful connection (or
// forever, if it never connects - callers should generally not block
// forever on this; see pushover_client.cpp for how it's used).
void wifi_connect_wait_for_ip(void);

bool wifi_connect_is_up(void);

// Forces a fresh connection attempt right now, on the *current* profile,
// independent of the event-driven reconnect/backoff above - a safety net
// for the connection watchdog in pushover_client.cpp, in case a stuck
// attempt somehow never produces a WIFI_EVENT_STA_DISCONNECTED to trigger
// the normal path.
void wifi_connect_force_reconnect(void);

// Forces a switch to the *next* configured profile and reconnects, even
// if currently associated (WiFi-level "connected") to a different one -
// for the case where the network itself is up but Pushover still isn't
// reachable through it (a captive portal, blocked ports, a dead upstream
// link, etc.), so retrying the same network endlessly wouldn't help.
// Called by pushover_client.cpp's watchdog. No-op if only one profile is
// configured.
void wifi_connect_try_next_profile(void);
