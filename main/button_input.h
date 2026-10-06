#pragma once

enum ButtonChannel { BTN_BOOT_ONLY, BTN_PWR_ONLY, BTN_BOTH };
enum ButtonGesture { GESTURE_SINGLE, GESTURE_DOUBLE, GESTURE_LONG };

struct ButtonEvent {
    ButtonChannel channel;
    ButtonGesture gesture;
};

// Starts a background FreeRTOS task that polls the BOOT and PWR buttons
// (debounced, ~20ms) and detects single-click / double-click / long-press
// for each button alone and for both held together. If the held
// combination changes mid-gesture (e.g. the second button joins in, or one
// releases while the other is still down), the in-progress gesture is
// discarded silently and a fresh one starts for the new combination -- so a
// combo press never also fires as two single-button presses.
//
// Every detected gesture is logged (ESP_LOGI, tag "button") for bring-up,
// and pushed onto an internal queue.
void button_input_start(void);

// Non-blocking: returns true and fills `out` if a gesture was queued.
bool button_input_receive(ButtonEvent &out);
