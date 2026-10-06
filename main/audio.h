#pragma once

// Brings up the ES8311 codec on this board (I2S + I2C, see codec_board's
// board_cfg.txt "S3_ePaper_1_54" entry). Call once at startup, after the
// audio supply rail is on (board_power_init()).
void audio_init(void);

// Plays one tone at the given frequency/duration (blocking). `amplitude`
// defaults to a loud level - the message alarm has to wake someone up;
// buzzer.cpp passes a lower value for its quiet disconnected warning.
// Safe to call from several tasks; the codec device is opened once and
// reused. No-op if audio_init() failed.
void audio_tone(float freq_hz, float duration_s, float amplitude = 28000.0f);
