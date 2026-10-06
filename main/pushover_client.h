#pragma once

// Pushover "Open Client" protocol: a persistent WebSocket to
// wss://client.pushover.net/push announces new mail with a bare '!',
// which triggers a REST fetch (GET messages.json) that stores the fetched
// messages locally for the buttons (buttons.cpp) to scroll through. Other
// protocol bytes: '#' keepalive, 'R' reconnect, 'E' fatal (see README,
// "How it works").
//
// Call once, after wifi_connect_start() and display_init().
void pushover_client_start(void);

// Message navigation, driven by buttons.cpp. Pushover has no "delete one
// message out of order" API - only a watermark ("everything up to this
// id is read"), so pushover_ack_current() acks (and so also silently
// drops from view) every stored message at or before the one currently
// shown, not just that one message.
void pushover_show_next(void);
void pushover_show_prev(void);
void pushover_ack_current(void);
