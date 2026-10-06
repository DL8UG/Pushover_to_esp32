#include <cstdlib>
#include <ctime>

#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include "user_config.h"
#include "board_power.h"
#include "display.h"
#include "wifi_connect.h"
#include "pushover_client.h"
#include "buzzer.h"
#include "buttons.h"

static const char *TAG = "main";

// SNTP has set (or corrected) the clock.
static void on_time_sync(struct timeval *tv)
{
    time_t t = tv->tv_sec;
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm);
    ESP_LOGI(TAG, "clock set via NTP: %s", buf);
    display_clock_changed();
}

extern "C" void app_main(void)
{
    // Only for showing times: Pushover's message dates and the SNTP clock
    // are both Unix time (UTC), and expiry compares those raw values, so
    // the time zone never enters that comparison.
    setenv("TZ", LOCAL_TIMEZONE, 1);
    tzset();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // Mandatory before any esp_netif_create_default_wifi_sta() call - without
    // it, lwIP's TCP/IP task/mailbox is never created, and the first real
    // WiFi frame after association hits "assert failed: tcpip_inpkt ...
    // (Invalid mbox)" and reboots.
    ESP_ERROR_CHECK(esp_netif_init());

    // Clock for message expiry (MESSAGE_EXPIRY_HOURS). Syncs in the
    // background once WiFi is up and keeps re-syncing; until the first
    // sync nothing expires (pushover_client.cpp checks for a valid time).
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER);
    sntp_cfg.sync_cb = on_time_sync;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_cfg));

    // EPD and audio codec each sit behind their own power-rail GPIO on this
    // board and stay off at reset - must be switched on before display_init()
    // or buzzer_init() touch either one.
    board_power_init();

    display_init();
    buzzer_init();
    buttons_init();
    wifi_connect_start();
    pushover_client_start();
}
