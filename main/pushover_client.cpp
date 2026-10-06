#include "pushover_client.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

#include "esp_websocket_client.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#include "secrets.h"
#include "user_config.h"
#include "wifi_connect.h"
#include "display.h"
#include "buzzer.h"
#include "ui.h"

namespace {

const char *TAG = "pushover";

// In PSRAM, not on the stack or in internal RAM: a real backlog (e.g. a
// monitoring feed) can be large - a 16 KB buffer was seen overflowing
// mid-message ("bad JSON, total_len=16383", i.e. truncated, not actually
// malformed), and a burst of ~1000 messages.json entries can run to a few
// hundred KB. A truncated response fails to parse (see the warning in
// fetch_and_store()), which could leave the backlog stuck too large to
// ever fetch. 512 KB from 8 MB of PSRAM leaves plenty of headroom.
constexpr size_t RX_BUF_SIZE = 524288;
char *s_rx_buf = nullptr;

esp_websocket_client_handle_t s_ws = nullptr;
TaskHandle_t s_fetch_task = nullptr;
volatile bool s_fatal = false; // set on a WS 'E' (bad secret/device) - stop retrying entirely

// Local copy of the fetched backlog, for the buttons to scroll through.
// Only what the screen shows is kept (the message body is not displayed,
// so it is not stored). PSRAM-backed like s_rx_buf.
struct StoredMsg {
    char id_str[32];
    int64_t date;    // Unix time the message was sent
    char title[160]; // UTF-8; the app name if the message has no title
    int priority;    // Pushover scale: -2 lowest .. 2 emergency, 0 default
};
// Only high-priority-and-above messages are stored (see MIN_PRIORITY in
// fetch_and_store()), so the backlog is realistically small. The whole
// store is persisted to NVS on every change (see persist_store()), so the
// cap also bounds the NVS blob size (~6 KB) against the 24 KB "nvs"
// partition shared with WiFi's own stored state.
constexpr int MAX_STORED_MSGS = 30;
StoredMsg *s_msgs = nullptr;
int s_msg_count = 0;
int s_cur_idx = 0; // which stored message is on screen; newest by default

// Guards s_msgs/s_msg_count/s_cur_idx - written from fetch_task (WS-
// triggered) and ack_current_task (button long-press), read/written from
// pushover_show_next/prev (button navigation, a third task), with no
// other ordering between them. Held only around the in-memory
// read/rebuild, never across a blocking HTTP call or a display redraw,
// so navigation stays responsive during a fetch.
SemaphoreHandle_t s_store_mutex = nullptr;

constexpr const char *NVS_NAMESPACE = "pushover";
constexpr const char *NVS_KEY_COUNT = "count";
// Renamed whenever StoredMsg changes layout, so a blob written by an older
// firmware is ignored instead of misread.
constexpr const char *NVS_KEY_MSGS = "msgs2";

// Persists the current high-priority store to NVS flash, so an undeleted
// alert survives a power loss/reset instead of only living in RAM. Called
// after every store change (new fetch or ack) - see fetch_and_store().
// Best-effort: a write failure is logged, not fatal, since losing the
// persisted copy just means falling back to an empty store on next boot,
// not losing the currently-running device state.
void persist_store(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;

    // Held across the NVS write too (not just the field reads) so a
    // concurrent fetch_and_store() (e.g. a WS-triggered fetch overlapping
    // a long-press ack's refetch, both call this) can never rewrite
    // s_msgs/s_msg_count mid-write here. NVS blob writes are a few KB at
    // most (MAX_STORED_MSGS is small) and fast, so this doesn't meaningfully
    // delay button navigation even in the rare case they do overlap.
    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    esp_err_t err = nvs_set_blob(h, NVS_KEY_MSGS, s_msgs, sizeof(StoredMsg) * (size_t)s_msg_count);
    if (err == ESP_OK) err = nvs_set_i32(h, NVS_KEY_COUNT, s_msg_count);
    if (err == ESP_OK) err = nvs_commit(h);
    xSemaphoreGive(s_store_mutex);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to persist message store: %s", esp_err_to_name(err));
    }
    nvs_close(h);
}

// Restores whatever was persisted before the last reset, if anything -
// called once at startup, before WiFi/Pushover even connect, so a
// power-loss-and-reboot shows the same undeleted alerts immediately.
void load_persisted_store(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    int32_t count = 0;
    if (nvs_get_i32(h, NVS_KEY_COUNT, &count) == ESP_OK && count > 0) {
        if (count > MAX_STORED_MSGS) count = MAX_STORED_MSGS;
        size_t len = sizeof(StoredMsg) * (size_t)count;
        size_t actual = len;
        // buttons_init() runs before pushover_client_start() in main.cpp, so
        // the button task already exists (though nothing to act on yet) -
        // take the lock here too rather than relying on the narrow timing.
        xSemaphoreTake(s_store_mutex, portMAX_DELAY);
        if (nvs_get_blob(h, NVS_KEY_MSGS, s_msgs, &actual) == ESP_OK && actual == len) {
            s_msg_count = count;
            s_cur_idx = s_msg_count - 1;
        }
        xSemaphoreGive(s_store_mutex);
        if (s_msg_count > 0) ESP_LOGI(TAG, "restored %d persisted message(s) after boot", s_msg_count);
    }
    nvs_close(h);
}

// True if a message sent at `date` has gone unacknowledged for longer than
// MESSAGE_EXPIRY_HOURS. Never true while the clock is not set or expiry is
// switched off. Both values are Unix time in UTC - Pushover's "date" and
// time() after SNTP - so no time zone conversion belongs here (TZ only
// affects localtime(), i.e. how ui.cpp shows the time).
bool is_expired(int64_t date, time_t now)
{
    if (MESSAGE_EXPIRY_HOURS <= 0 || !clock_is_set(now) || date <= 0) return false;
    return date < (int64_t)now - (int64_t)MESSAGE_EXPIRY_HOURS * 3600;
}

// cJSON's default allocator is plain malloc()/free(). This project's
// sdkconfig sets CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384, meaning any
// single malloc() below 16 KB - which is every individual cJSON node,
// there are hundreds of small ones for a real backlog - is forced into
// scarce internal RAM regardless of PSRAM being available - a classic
// internal-RAM-exhaustion crash on these boards. A backlog of ~1000 messages could plausibly need several
// hundred KB of node storage, which internal RAM (a few hundred KB
// total, shared with WiFi/TLS/everything else) does not have. Point
// cJSON at PSRAM explicitly instead, once, at startup.
void *cjson_psram_malloc(size_t sz) { return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM); }
void cjson_psram_free(void *ptr) { heap_caps_free(ptr); }

struct RxCtx {
    char *buf;
    size_t len;
    size_t cap;
};

esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        RxCtx *ctx = static_cast<RxCtx *>(evt->user_data);
        size_t room = ctx->cap - 1 - ctx->len;
        size_t n = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
        if (n > 0) {
            memcpy(ctx->buf + ctx->len, evt->data, n);
            ctx->len += n;
        }
    }
    return ESP_OK;
}

// Shared GET/POST helper: response body into rx_buf (null-terminated,
// truncated to rx_cap), true on HTTP 200.
bool http_call(const char *url, esp_http_client_method_t method,
               const char *post_body, char *rx_buf, size_t rx_cap)
{
    RxCtx ctx = { rx_buf, 0, rx_cap };
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = method;
    config.event_handler = http_event_handler;
    config.user_data = &ctx;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 10000;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (post_body) {
        esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(client, post_body, (int)strlen(post_body));
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "http call failed: %s", esp_err_to_name(err));
        return false;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "http call returned status %d", status);
        return false;
    }
    rx_buf[ctx.len] = '\0';
    return true;
}

// highest_id as a STRING, not int/int64_t: Pushover's real message ids
// (hardware-verified, e.g. 1175816417220301065) exceed not just INT32_MAX
// but a double's exact-integer range too (over 2^53), so cJSON's own
// numeric "id" field has already lost precision by the time it's parsed -
// confirmed on hardware as "acked up to message 2147483647" (silent
// int overflow to INT_MAX). Pushover ships a same-value "id_str" field
// specifically to avoid this (the same pattern Twitter/X's API uses for
// tweet ids) - use that instead, as a string, all the way through.
void ack_highest(const char *highest_id_str)
{
    char url[160];
    snprintf(url, sizeof(url), "https://api.pushover.net/1/devices/%s/update_highest_message.json",
             PUSHOVER_DEVICE_ID);
    char body[192];
    snprintf(body, sizeof(body), "secret=%s&message=%s", PUSHOVER_SECRET, highest_id_str);

    char ack_buf[512];
    if (!http_call(url, HTTP_METHOD_POST, body, ack_buf, sizeof(ack_buf))) {
        ESP_LOGW(TAG, "ack failed for message %s - it may be re-shown next fetch", highest_id_str);
        return;
    }
    ESP_LOGI(TAG, "acked up to message %s", highest_id_str);
}

// Highest message id already logged since boot (RAM-only - a serial debug
// log, not a persisted history). Pushover keeps returning every not-yet-acked message on
// every fetch, so without this a message would get logged again on each
// subsequent fetch until it's acked - this dedupes across fetches, not
// just within one, by remembering the high-water mark. int64_t / strtoll,
// not the numeric "id" field: same precision issue as ack_highest()'s
// id_str (Pushover ids exceed a double's exact-integer range).
int64_t s_last_logged_id = 0;

void log_new_message(const char *id_str, int priority, const char *title)
{
    int64_t id = id_str ? strtoll(id_str, nullptr, 10) : 0;
    if (id <= s_last_logged_id) return; // already logged this one on an earlier fetch
    s_last_logged_id = id;
    ESP_LOGI(TAG, "received message id=%s priority=%d title=\"%s\"", id_str ? id_str : "?", priority, title);
}

// Copies UTF-8 `src` into `dst`, cutting only between characters; a cut
// title ends in "…" so it does not look complete on screen.
void copy_utf8(char *dst, size_t size, const char *src)
{
    size_t len = strlen(src);
    if (len < size) {
        memcpy(dst, src, len + 1);
        return;
    }
    static const char ELLIPSIS[] = "\xE2\x80\xA6";
    size_t cut = size - sizeof(ELLIPSIS); // room for "…" and '\0'
    while (cut > 0 && (src[cut] & 0xC0) == 0x80) cut--; // not inside a character
    memcpy(dst, src, cut);
    memcpy(dst + cut, ELLIPSIS, sizeof(ELLIPSIS));
}

// Switches the display into (or refreshes) the browsing view for
// whatever's currently in s_msgs/s_cur_idx - no network I/O. Called only
// for user-driven navigation (next/prev/ack), not for a background fetch -
// an alert arriving in the background shouldn't yank the screen away from
// whatever the user is looking at, it just updates the idle count (see
// fetch_and_store()) for whenever the view falls back there on its own.
void show_current_or_idle(void)
{
    char title[sizeof(StoredMsg::title)];
    int64_t date = 0;
    int priority = 0;
    int idx = 0, count;

    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    count = s_msg_count;
    if (count > 0) {
        if (s_cur_idx < 0) s_cur_idx = 0;
        if (s_cur_idx >= s_msg_count) s_cur_idx = s_msg_count - 1;
        idx = s_cur_idx;
        memcpy(title, s_msgs[idx].title, sizeof(title)); // already null-terminated by fetch_and_store()
        date = s_msgs[idx].date;
        priority = s_msgs[idx].priority;
    }
    xSemaphoreGive(s_store_mutex);

    if (count == 0) {
        display_show_idle();
    } else {
        display_show_message(title, date, priority, idx, count);
    }
}

// GET the current backlog and replace the local store with it. Does NOT
// ack anything - that only happens via pushover_ack_current(), driven by
// the buttons' long-press (see pushover_client.h). Returns true if the
// fetch itself succeeded (even if it returned zero messages).
bool fetch_and_store(void)
{
    char url[192];
    snprintf(url, sizeof(url), "https://api.pushover.net/1/messages.json?secret=%s&device_id=%s",
             PUSHOVER_SECRET, PUSHOVER_DEVICE_ID);

    if (!http_call(url, HTTP_METHOD_GET, nullptr, s_rx_buf, RX_BUF_SIZE)) {
        ESP_LOGW(TAG, "fetch failed");
        return false;
    }

    cJSON *root = cJSON_Parse(s_rx_buf);
    if (!root) {
        size_t len = strlen(s_rx_buf);
        const char *err = cJSON_GetErrorPtr();
        size_t err_off = err ? (size_t)(err - s_rx_buf) : 0;
        ESP_LOGW(TAG, "fetch: bad JSON, total_len=%u, error near offset %u:", (unsigned)len, (unsigned)err_off);
        size_t start = err_off > 100 ? err_off - 100 : 0;
        ESP_LOGW(TAG, "context: %.200s", s_rx_buf + start);
        return false;
    }
    cJSON *messages = cJSON_GetObjectItemCaseSensitive(root, "messages");
    if (!cJSON_IsArray(messages)) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *item = nullptr;

    // Only high-priority-and-above messages are kept - routine/normal
    // traffic (e.g. a monitoring feed's routine noise) is fetched but
    // otherwise ignored entirely: not stored, not shown, not counted, not
    // buzzed. Pushover's own scale: -2 lowest .. 2 emergency, 1 = high.
    constexpr int MIN_PRIORITY = 1;

    // Oldest-first in the response. First pass: count how many are kept
    // (high priority and not expired - for the store-overflow skip below),
    // and find the auto-ack watermark - the highest id_str of the leading
    // run of messages that are not kept, i.e. everything up to (but not
    // including) the first one to show. Low-priority and expired messages
    // are cleared from Pushover's own backlog right away since they're
    // never kept locally anyway; a message to show is only ever acked by
    // the user (long press) or by expiring, so the scan must stop at the
    // first one - acking past it would silently drop it from Pushover's
    // queue before anyone saw it.
    const time_t now = time(nullptr);
    int total_high = 0;
    int expired = 0;
    const char *safe_ack_id_str = nullptr;
    bool hit_high = false;
    cJSON_ArrayForEach(item, messages) {
        const cJSON *priority = cJSON_GetObjectItemCaseSensitive(item, "priority");
        const cJSON *id_str = cJSON_GetObjectItemCaseSensitive(item, "id_str");
        const cJSON *title = cJSON_GetObjectItemCaseSensitive(item, "title");
        int p = cJSON_IsNumber(priority) ? priority->valueint : 0;

        // Serial debug log of every message received, regardless of the
        // priority filter below (see log_new_message()'s comment).
        log_new_message(cJSON_IsString(id_str) ? id_str->valuestring : nullptr, p,
                         (cJSON_IsString(title) && title->valuestring[0]) ? title->valuestring : "(no title)");

        const cJSON *date = cJSON_GetObjectItemCaseSensitive(item, "date");
        bool too_old = is_expired(cJSON_IsNumber(date) ? (int64_t)date->valuedouble : 0, now);
        if (p >= MIN_PRIORITY && too_old) expired++;

        if (p >= MIN_PRIORITY && !too_old) {
            total_high++;
            hit_high = true;
        } else if (!hit_high) {
            if (cJSON_IsString(id_str)) safe_ack_id_str = id_str->valuestring;
        }
    }
    if (expired > 0) {
        ESP_LOGI(TAG, "%d message(s) unacknowledged for over %d h - expired", expired, MESSAGE_EXPIRY_HOURS);
    }
    if (safe_ack_id_str) {
        ack_highest(safe_ack_id_str);
    }

    // Keep only the newest MAX_STORED_MSGS *matching* entries - a real
    // backlog can exceed the store, and the newest ones are the ones
    // worth keeping. Second pass fills the store applying that skip
    // against the filtered count from above.
    int skip = total_high > MAX_STORED_MSGS ? total_high - MAX_STORED_MSGS : 0;
    int seen = 0;
    int new_count = 0;

    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    cJSON_ArrayForEach(item, messages) {
        const cJSON *priority = cJSON_GetObjectItemCaseSensitive(item, "priority");
        int p = cJSON_IsNumber(priority) ? priority->valueint : 0;
        if (p < MIN_PRIORITY) continue;
        const cJSON *date = cJSON_GetObjectItemCaseSensitive(item, "date");
        if (is_expired(cJSON_IsNumber(date) ? (int64_t)date->valuedouble : 0, now)) continue;
        if (seen++ < skip) continue;

        const cJSON *id_str = cJSON_GetObjectItemCaseSensitive(item, "id_str");
        const cJSON *title = cJSON_GetObjectItemCaseSensitive(item, "title");
        const cJSON *app = cJSON_GetObjectItemCaseSensitive(item, "app");
        StoredMsg *m = &s_msgs[new_count++];
        m->id_str[0] = '\0';
        if (cJSON_IsString(id_str)) {
            strncpy(m->id_str, id_str->valuestring, sizeof(m->id_str) - 1);
            // strncpy() does not null-terminate when the source is >= the
            // destination size - real Pushover ids never reach 31 chars, but
            // don't leave a garbage-read footgun in place for whenever that
            // assumption stops holding.
            m->id_str[sizeof(m->id_str) - 1] = '\0';
        }
        const char *t = (cJSON_IsString(title) && title->valuestring[0]) ? title->valuestring
                        : (cJSON_IsString(app) && app->valuestring[0]) ? app->valuestring
                        : "(no title)";
        copy_utf8(m->title, sizeof(m->title), t);
        m->date = cJSON_IsNumber(date) ? (int64_t)date->valuedouble : 0;
        m->priority = p;
    }
    s_msg_count = new_count;
    s_cur_idx = s_msg_count - 1; // newest
    xSemaphoreGive(s_store_mutex);

    cJSON_Delete(root);

    // Background fetch: only update the home screen's count, never yank
    // the display into the browsing view out from under the user - see
    // show_current_or_idle()'s comment.
    display_set_message_count(s_msg_count);
    // Alarm runs for as long as anything undeleted remains, regardless of
    // whether this particular fetch added to it - covers both "new mail
    // arrived" (0 -> >0) and "still not dealt with" after a reboot or a
    // keepalive-triggered refresh, and stops itself the moment the count
    // returns to 0 via ack.
    buzzer_set_alarm(s_msg_count > 0);
    persist_store();
    return true;
}

void fetch_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ulTaskNotifyTake(pdFALSE, 0); // drain any that piled up
        fetch_and_store();
    }
}

// esp_websocket_client_stop()/_start() cannot be called from inside the WS
// event handler (it runs on the client's own task) - these run it from a
// freshly spawned one-shot task instead, per the docs' explicit warning.
void stop_task(void *arg)
{
    (void)arg;
    esp_websocket_client_stop(s_ws);
    vTaskDelete(nullptr);
}

void restart_task(void *arg)
{
    (void)arg;
    esp_websocket_client_stop(s_ws);
    esp_websocket_client_start(s_ws);
    vTaskDelete(nullptr);
}

void ws_event_handler(void *handler_arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_arg;
    (void)base;
    auto *data = static_cast<esp_websocket_event_data_t *>(event_data);

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "WS connected, sending login");
        // Exact format per pushover.net/api/client's "Real-Time Message
        // Notification" section: "login:" + device id + ":" + secret + \n.
        // NOT "login=...&secret=..." (that was the original, wrong,
        // HTTP-query-string-shaped guess - it can never have worked, the
        // server has no "login=" key to parse).
        char login[128];
        int n = snprintf(login, sizeof(login), "login:%s:%s\n", PUSHOVER_DEVICE_ID, PUSHOVER_SECRET);
        esp_websocket_client_send_text(s_ws, login, n, portMAX_DELAY);
        display_set_pushover_status(true);
        break;
    }
    case WEBSOCKET_EVENT_DATA:
        if (data->data_len >= 1) {
            char c = data->data_ptr[0];
            switch (c) {
            case '!': // new message waiting
                if (s_fetch_task) xTaskNotifyGive(s_fetch_task);
                break;
            case '#': // keepalive
                break;
            case 'R': // server wants a fresh reconnect
                ESP_LOGI(TAG, "server requested reconnect");
                // esp_websocket_client_stop() "cannot be called from the
                // websocket event handler" (confirmed on hardware: doing
                // so logged "Client cannot be stopped from websocket
                // task") - hand it to a one-shot task instead.
                xTaskCreate(restart_task, "ws_restart", 3072, nullptr, 5, nullptr);
                break;
            case 'E': // fatal - bad secret/device_id, or device deleted
                ESP_LOGE(TAG, "fatal WS error from server - stopping, will not retry");
                s_fatal = true;
                display_set_pushover_status(false);
                xTaskCreate(stop_task, "ws_stop", 3072, nullptr, 5, nullptr);
                break;
            default:
                ESP_LOGW(TAG, "unexpected WS byte: 0x%02x", (unsigned)c);
                break;
            }
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (!s_fatal) display_set_pushover_status(false);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WS error event");
        break;
    default:
        break;
    }
}

// Drops expired messages from the local store (they are older than all
// others, so always at its front). Returns how many were dropped. On
// Pushover they stay until the next fetch, which acknowledges them.
int prune_expired(void)
{
    const time_t now = time(nullptr);
    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    int n = 0;
    while (n < s_msg_count && is_expired(s_msgs[n].date, now)) n++;
    if (n > 0) {
        memmove(s_msgs, s_msgs + n, sizeof(StoredMsg) * (size_t)(s_msg_count - n));
        s_msg_count -= n;
        s_cur_idx = s_cur_idx >= n ? s_cur_idx - n : 0;
        if (s_cur_idx >= s_msg_count) s_cur_idx = s_msg_count - 1;
        if (s_cur_idx < 0) s_cur_idx = 0;
    }
    int count = s_msg_count;
    xSemaphoreGive(s_store_mutex);

    if (n > 0) {
        ESP_LOGI(TAG, "%d stored message(s) unacknowledged for over %d h - expired", n, MESSAGE_EXPIRY_HOURS);
        persist_store();
        display_set_message_count(count);
        buzzer_set_alarm(count > 0);
    }
    return n;
}

// Polls WiFi + Pushover WS connectivity every WATCHDOG_INTERVAL_MS and
// mirrors it onto the home screen. Both wifi_connect.cpp's own
// disconnect-driven backoff and esp_websocket_client's built-in
// auto-reconnect already handle the common case; this is a second,
// time-based safety net for the rarer case where a stuck attempt never
// produces the event that would normally trigger a retry - if either
// link has been down continuously past its timeout, force one.
constexpr int64_t WATCHDOG_INTERVAL_MS = 2000;
constexpr int64_t WIFI_STUCK_TIMEOUT_MS = 30000;
// How often to retry recovery actions (WS restart / next WiFi profile)
// while Pushover stays unreachable - independent of, and much shorter
// than, DISCONNECTED_WARNING_MS below, which tracks *total* continuous
// downtime and must NOT get reset every time a recovery action fires.
constexpr int64_t WS_RECOVERY_INTERVAL_MS = 60000;
constexpr int64_t DISCONNECTED_WARNING_MS = 15 * 60 * 1000;
// How often stored messages are checked for expiry (MESSAGE_EXPIRY_HOURS).
constexpr int64_t EXPIRY_CHECK_INTERVAL_MS = 60 * 1000;

void watchdog_task(void *arg)
{
    (void)arg;
    int64_t wifi_down_since = 0;
    int64_t ws_down_since = 0;         // 0 = currently up; else when it was first seen down
    int64_t last_recovery_attempt_ms = 0;
    int64_t last_expiry_check_ms = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_INTERVAL_MS));
        int64_t now = esp_timer_get_time() / 1000;

        // Expired messages also go away without any new traffic; the
        // fetch then acknowledges them on Pushover too.
        if (now - last_expiry_check_ms >= EXPIRY_CHECK_INTERVAL_MS) {
            last_expiry_check_ms = now;
            if (prune_expired() > 0 && s_fetch_task) xTaskNotifyGive(s_fetch_task);
        }

        bool wifi_up = wifi_connect_is_up();
        display_set_wifi_status(wifi_up);
        if (!wifi_up) {
            if (wifi_down_since == 0) {
                wifi_down_since = now;
            } else if (now - wifi_down_since > WIFI_STUCK_TIMEOUT_MS) {
                ESP_LOGW(TAG, "watchdog: WiFi stuck down, forcing reconnect");
                wifi_connect_force_reconnect();
                wifi_down_since = now; // don't force again every tick
            }
        } else {
            wifi_down_since = 0;
        }

        bool ws_up = !s_fatal && s_ws && esp_websocket_client_is_connected(s_ws);
        if (ws_up) {
            if (ws_down_since != 0) {
                ws_down_since = 0;
                buzzer_set_disconnected_warning(false);
                display_set_pushover_unreachable(false);
            }
        } else {
            if (ws_down_since == 0) {
                ws_down_since = now;
                last_recovery_attempt_ms = now;
            }

            // Recovery actions (WS restart / next WiFi profile) only make
            // sense for a transient failure, not a fatal one (bad
            // secret/device - reflashing new credentials is the only
            // fix). Periodic, not every 2s tick. If WiFi itself is down,
            // wifi_connect.cpp's own disconnect-driven profile round-robin
            // is already actively retrying - nothing extra to do here
            // until it reports wifi_up again.
            if (!s_fatal && wifi_up && now - last_recovery_attempt_ms >= WS_RECOVERY_INTERVAL_MS) {
                last_recovery_attempt_ms = now;
                ESP_LOGW(TAG, "watchdog: Pushover unreachable while WiFi is up - trying next WiFi profile + WS restart");
                wifi_connect_try_next_profile();
                xTaskCreate(restart_task, "ws_restart_wd", 3072, nullptr, 5, nullptr);
            }

            // The 15-minute warning itself applies either way - a fatal
            // auth error is just as much "no connection to Pushover
            // possible" as a transient one, and arguably more worth
            // surfacing since it won't recover on its own.
            if (now - ws_down_since >= DISCONNECTED_WARNING_MS) {
                buzzer_set_disconnected_warning(true);
                display_set_pushover_unreachable(true);
            }
        }
    }
}

void pushover_task(void *arg)
{
    (void)arg;
    wifi_connect_wait_for_ip();

    xTaskCreate(fetch_task, "pushover_fetch", 6144, nullptr, 4, &s_fetch_task);

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = "wss://client.pushover.net/push";
    ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    // Pushover's own keepalive cadence is documented around every 60s of
    // idle - ping this a bit more often so a silently-dead link is
    // detected and reconnected well before that.
    ws_cfg.reconnect_timeout_ms = 10000;
    ws_cfg.network_timeout_ms = 15000;

    s_ws = esp_websocket_client_init(&ws_cfg);
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, nullptr);
    esp_websocket_client_start(s_ws);

    xTaskCreate(watchdog_task, "conn_watchdog", 3072, nullptr, 3, nullptr);

    vTaskDelete(nullptr);
}

// Runs the ack + refetch on its own one-shot task, so a long-press doesn't
// block the button-polling loop for the duration of two blocking HTTP
// calls (ack_highest() then fetch_and_store(), each up to the 10s
// network timeout).
void ack_current_task(void *arg)
{
    (void)arg;
    char id_str[sizeof(StoredMsg::id_str)];
    id_str[0] = '\0';

    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    if (s_msg_count > 0) {
        memcpy(id_str, s_msgs[s_cur_idx].id_str, sizeof(id_str));
    }
    xSemaphoreGive(s_store_mutex);

    if (id_str[0]) {
        ack_highest(id_str);
    }
    fetch_and_store();
    show_current_or_idle(); // user-driven (long press) - stay in the browsing view
    vTaskDelete(nullptr);
}

} // namespace

void pushover_show_next(void)
{
    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    if (s_cur_idx < s_msg_count - 1) s_cur_idx++;
    xSemaphoreGive(s_store_mutex);
    show_current_or_idle();
}

void pushover_show_prev(void)
{
    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    if (s_cur_idx > 0) s_cur_idx--;
    xSemaphoreGive(s_store_mutex);
    show_current_or_idle();
}

void pushover_ack_current(void)
{
    xSemaphoreTake(s_store_mutex, portMAX_DELAY);
    bool empty = s_msg_count == 0;
    xSemaphoreGive(s_store_mutex);
    if (empty) return; // nothing to ack - skip the task + network fetch entirely

    xTaskCreate(ack_current_task, "pushover_ack", 6144, nullptr, 4, nullptr);
}

void pushover_client_start(void)
{
    cJSON_Hooks hooks = {};
    hooks.malloc_fn = cjson_psram_malloc;
    hooks.free_fn = cjson_psram_free;
    cJSON_InitHooks(&hooks);

    s_store_mutex = xSemaphoreCreateMutex();

    s_msgs = static_cast<StoredMsg *>(heap_caps_malloc(sizeof(StoredMsg) * MAX_STORED_MSGS, MALLOC_CAP_SPIRAM));
    if (!s_msgs) {
        ESP_LOGE(TAG, "failed to allocate message store");
        return;
    }
    s_rx_buf = static_cast<char *>(heap_caps_malloc(RX_BUF_SIZE, MALLOC_CAP_SPIRAM));
    if (!s_rx_buf) {
        ESP_LOGE(TAG, "failed to allocate %u byte PSRAM rx buffer", (unsigned)RX_BUF_SIZE);
        return;
    }

    // Restore whatever survived the last reset before anything else, so
    // the home screen shows the right count immediately - even before
    // WiFi/Pushover reconnect, which can take a few seconds.
    load_persisted_store();
    display_set_message_count(s_msg_count);
    buzzer_set_alarm(s_msg_count > 0); // still-undeleted alerts should keep alarming across a reboot too

    xTaskCreate(pushover_task, "pushover_start", 4096, nullptr, 5, nullptr);
}
