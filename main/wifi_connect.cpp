#include "wifi_connect.h"

#include <cstring>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "secrets.h"

namespace {

const char *TAG = "wifi_connect";
constexpr int CONNECTED_BIT = BIT0;

struct WifiProfile {
    const char *ssid;
    const char *pass;
};

// Built from secrets.h's numbered WIFI_SSID_n/WIFI_PASS_n pairs, skipping
// any with an empty SSID - so a user who only fills in one slot still
// gets a one-profile list, not two empty ones the round-robin would
// pointlessly cycle through.
WifiProfile s_profiles[3];
int s_profile_count = 0;
int s_current_profile_idx = 0;

EventGroupHandle_t s_event_group = nullptr;
esp_timer_handle_t s_reconnect_timer = nullptr;
int s_fail_count = 0;
bool s_connected = false;

void build_profile_list(void)
{
    const WifiProfile candidates[] = {
        { WIFI_SSID_1, WIFI_PASS_1 },
        { WIFI_SSID_2, WIFI_PASS_2 },
        { WIFI_SSID_3, WIFI_PASS_3 },
    };
    for (const auto &c : candidates) {
        if (c.ssid && c.ssid[0]) s_profiles[s_profile_count++] = c;
    }
    if (s_profile_count == 0) {
        ESP_LOGE(TAG, "no WiFi profiles configured in secrets.h - fill in at least WIFI_SSID_1");
    }
}

// Loads profile `idx`'s SSID/password into the driver. Does not itself
// connect - callers issue esp_wifi_connect() (or let the reconnect timer
// do it) afterward.
void apply_profile(int idx)
{
    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char *>(wifi_config.sta.ssid), s_profiles[idx].ssid,
                 sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char *>(wifi_config.sta.password), s_profiles[idx].pass,
                 sizeof(wifi_config.sta.password) - 1);
    // Weakest/most permissive threshold rather than hardcoding WPA2 - the
    // configured profiles may span different security types (open, WPA2,
    // WPA3...) and this is only a *minimum accepted* filter, not the
    // actual handshake, which still uses each profile's own password.
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    // Wake on every beacon (~100ms typical) rather than the driver's
    // default of 3 (~307ms) - see esp_wifi_set_ps() below: this is the
    // other half of the power/latency balance for eventual power-bank
    // operation, capping the worst-case wake delay much closer to
    // WIFI_PS_NONE's zero while still letting the radio duty-cycle.
    wifi_config.sta.listen_interval = 1;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI(TAG, "using WiFi profile %d/%d: %s", idx + 1, s_profile_count, s_profiles[idx].ssid);
}

void reconnect_timer_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

void schedule_reconnect(void)
{
    // 1s, 2s, 4s, 8s, 16s, capped at 30s - a router that sees rapid-fire
    // reconnect attempts can start throttling/blacklisting the client MAC,
    // which then never recovers because the retries never stop long enough
    // for that to expire. Applies the same way
    // whether we're retrying the same profile or one just switched to.
    int shift = s_fail_count - 1;
    if (shift < 0) shift = 0;
    if (shift > 4) shift = 4;
    uint64_t delay_us = (uint64_t)1000000 << shift;
    if (delay_us > 30000000) delay_us = 30000000;
    esp_timer_stop(s_reconnect_timer); // no-op if not running
    esp_timer_start_once(s_reconnect_timer, delay_us);
}

void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        xEventGroupClearBits(s_event_group, CONNECTED_BIT);
        s_fail_count++;
        // Round-robin to the next configured profile on every failure
        // (not just after several retries on the same one), so a working
        // network is found as soon as possible.
        if (s_profile_count > 1) {
            s_current_profile_idx = (s_current_profile_idx + 1) % s_profile_count;
            apply_profile(s_current_profile_idx);
        }
        ESP_LOGW(TAG, "disconnected (fail_count=%d), scheduling reconnect", s_fail_count);
        schedule_reconnect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        s_fail_count = 0;
        ESP_LOGI(TAG, "connected");
        xEventGroupSetBits(s_event_group, CONNECTED_BIT);
    }
}

} // namespace

void wifi_connect_start(void)
{
    build_profile_list();

    s_event_group = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, nullptr));

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = &reconnect_timer_cb;
    timer_args.name = "wifi_reconnect";
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (s_profile_count > 0) {
        apply_profile(0);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    // Power/latency balance for eventual power-bank operation: modem-sleep
    // (the ESP-IDF default) lets the radio duty-cycle off between wake
    // points, worth real battery life over WIFI_PS_NONE - but paired with
    // listen_interval=1 above (was the driver's default of 3, ~307ms) so
    // the worst-case wake delay for inbound WS traffic stays close to one
    // beacon interval (~100ms) instead of three. A very delayed notification
    // was traced back to the *combination* of power save + the longer
    // listen interval, not power save alone.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
}

void wifi_connect_wait_for_ip(void)
{
    xEventGroupWaitBits(s_event_group, CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

bool wifi_connect_is_up(void)
{
    return s_connected;
}

void wifi_connect_force_reconnect(void)
{
    esp_wifi_connect(); // harmless if already connected/connecting - errors ignored
}

void wifi_connect_try_next_profile(void)
{
    if (s_profile_count <= 1) return; // nothing else to switch to

    s_current_profile_idx = (s_current_profile_idx + 1) % s_profile_count;
    apply_profile(s_current_profile_idx);
    ESP_LOGW(TAG, "WiFi up but Pushover unreachable - forcing switch to profile %d/%d",
             s_current_profile_idx + 1, s_profile_count);
    // Disconnect explicitly first: esp_wifi_set_config() alone doesn't
    // tear down an existing association, and we want to actually leave
    // the current (WiFi-level-fine, Pushover-unreachable) network rather
    // than just queue the new credentials for whenever it next drops on
    // its own.
    esp_wifi_disconnect();
    esp_wifi_connect();
}
