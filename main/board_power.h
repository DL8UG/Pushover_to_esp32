#pragma once

// Switches on the e-paper and audio codec supply rails. They are off after
// a reset; call before display_init() and buzzer_init().
void board_power_init(void);
