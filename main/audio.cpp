#include "audio.h"
#include "codec_board.h"
#include "codec_init.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cmath>

namespace {

const char *TAG = "audio";
esp_codec_dev_handle_t s_playback = nullptr;
bool s_opened = false;
// The message alarm and the disconnected warning (buzzer.cpp) play from
// different tasks - esp_codec_dev isn't guaranteed re-entrant, so
// serialize access to it.
SemaphoreHandle_t s_mutex = nullptr;

constexpr int SAMPLE_RATE = 16000;

bool ensure_opened() {
    if (!s_playback) return false;
    if (s_opened) return true;

    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = SAMPLE_RATE;
    fs.channel = 2;
    fs.bits_per_sample = 16;
    esp_codec_dev_set_out_vol(s_playback, 100.0);
    if (esp_codec_dev_open(s_playback, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Failed to open codec for playback");
        return false;
    }
    s_opened = true;
    return true;
}

// Plays a tone with a quick ramp in/out to avoid a click at the edges.
void play_tone(float freq_hz, float duration_s, float amplitude) {
    if (!s_mutex || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return;

    if (!ensure_opened()) {
        xSemaphoreGive(s_mutex);
        return;
    }

    const int n = (int)(SAMPLE_RATE * duration_s);
    const int ramp = SAMPLE_RATE / 100; // 10ms

    auto *buf = (int16_t *)heap_caps_malloc(n * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        xSemaphoreGive(s_mutex);
        return;
    }

    for (int i = 0; i < n; i++) {
        float t = (float)i / SAMPLE_RATE;
        float env = 1.0f;
        if (i < ramp) env = (float)i / ramp;
        else if (i > n - ramp) env = (float)(n - i) / ramp;
        auto s = (int16_t)(env * amplitude * sinf(2.0f * (float)M_PI * freq_hz * t));
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }

    esp_codec_dev_write(s_playback, buf, n * 2 * sizeof(int16_t));
    heap_caps_free(buf);
    xSemaphoreGive(s_mutex);
}

} // namespace

void audio_init(void) {
    s_mutex = xSemaphoreCreateMutex();

    set_codec_board_type("S3_ePaper_1_54");

    // Board wires the ES8311 as a single full-duplex ("in_out") codec, so
    // both directions get initialized together even though we only play.
    codec_init_cfg_t codec_cfg = {};
    codec_cfg.in_mode = CODEC_I2S_MODE_STD;
    codec_cfg.out_mode = CODEC_I2S_MODE_STD;
    codec_cfg.in_use_tdm = false;
    codec_cfg.reuse_dev = false;
    if (init_codec(&codec_cfg) != 0) {
        ESP_LOGW(TAG, "init_codec failed, alarm tones will be unavailable");
        return;
    }

    s_playback = get_playback_handle();
    if (!s_playback) {
        ESP_LOGW(TAG, "No playback handle, alarm tones will be unavailable");
    }
}

void audio_tone(float freq_hz, float duration_s, float amplitude) {
    play_tone(freq_hz, duration_s, amplitude);
}
