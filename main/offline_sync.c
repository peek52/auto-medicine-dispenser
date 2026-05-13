#include "offline_sync.h"

#include "cloud_secrets.h"
#include "netpie_mqtt.h"
#include "wifi_sta.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "offline_sync";
static const TickType_t OFFLINE_RETRY_INTERVAL = pdMS_TO_TICKS(5000);

#define OFFLINE_SYNC_NAMESPACE "offline_q"
// Both queues must fit in a single NVS blob (~4 KB max each). Previous
// sizes (24 items × 1025 bytes = 24 600 bytes for the event queue)
// busted the limit and made every save fail with NVS_VALUE_TOO_LONG —
// the offline queue effectively wasn't persistent across reboots even
// though the in-RAM ring was working. Shrunk for tonight; per-key
// storage in a future refactor will lift the cap cleanly.
//
//   shadow item =                            192 bytes × 20 = 3 840
//   event  item = 1 + 384 + 128 + 128 = 641 bytes ×  6 = 3 846
#define OFFLINE_MAX_SHADOW_ITEMS 20
#define OFFLINE_MAX_EVENT_ITEMS   6
#define OFFLINE_SHADOW_PAYLOAD_LEN 192
#define OFFLINE_EVENT_TEXT_LEN   384
#define OFFLINE_EVENT_FIELD_LEN  128

typedef struct {
    char payload[OFFLINE_SHADOW_PAYLOAD_LEN];
} offline_shadow_item_t;

typedef enum {
    OFFLINE_EVENT_NONE = 0,
    OFFLINE_EVENT_TG_TEXT = 1,
    OFFLINE_EVENT_GSHEET = 2,
} offline_event_type_t;

typedef struct {
    uint8_t type;
    char a[OFFLINE_EVENT_TEXT_LEN];
    char b[OFFLINE_EVENT_FIELD_LEN];
    char c[OFFLINE_EVENT_FIELD_LEN];
} offline_event_item_t;

static offline_shadow_item_t s_shadow_queue[OFFLINE_MAX_SHADOW_ITEMS];
static offline_event_item_t s_event_queue[OFFLINE_MAX_EVENT_ITEMS];
static size_t s_shadow_count = 0;
static size_t s_event_count = 0;
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_flush_task = NULL;
static TaskHandle_t s_watch_task = NULL;
static char s_last_gsheet_event[OFFLINE_EVENT_TEXT_LEN] = "";
static char s_last_gsheet_meds[OFFLINE_EVENT_FIELD_LEN] = "";
static char s_last_gsheet_detail[OFFLINE_EVENT_FIELD_LEN] = "";
static TickType_t s_last_gsheet_tick = 0;

static bool offline_sync_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_lock) return false;
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(250)) == pdTRUE;
}

static void offline_sync_unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

static void copy_trunc(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    if (!src) src = "";
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static bool event_type_supported(uint8_t type)
{
    return type == OFFLINE_EVENT_TG_TEXT || type == OFFLINE_EVENT_GSHEET;
}

static bool gsheet_event_enabled(const char *event)
{
    if (!event || !event[0]) return false;
    return strcmp(event, "Dispensed") == 0 ||
           strcmp(event, "Partial Dispense") == 0 ||
           strcmp(event, "Not Dispensed") == 0 ||
           strcmp(event, "Skipped (Timeout)") == 0 ||
           strcmp(event, "Error - Not Dropped") == 0;
}

static bool gsheet_status_ok(int status)
{
    return status >= 200 && status < 400;
}

static bool gsheet_event_matches_last(const char *event, const char *meds, const char *detail)
{
    const TickType_t kDuplicateWindow = pdMS_TO_TICKS(3000);
    TickType_t now = xTaskGetTickCount();
    if (s_last_gsheet_tick == 0) return false;
    if ((now - s_last_gsheet_tick) > kDuplicateWindow) return false;
    return strcmp(s_last_gsheet_event, event ? event : "-") == 0 &&
           strcmp(s_last_gsheet_meds, meds ? meds : "-") == 0 &&
           strcmp(s_last_gsheet_detail, detail ? detail : "-") == 0;
}

static void gsheet_mark_recent(const char *event, const char *meds, const char *detail)
{
    copy_trunc(s_last_gsheet_event, sizeof(s_last_gsheet_event), event ? event : "-");
    copy_trunc(s_last_gsheet_meds, sizeof(s_last_gsheet_meds), meds ? meds : "-");
    copy_trunc(s_last_gsheet_detail, sizeof(s_last_gsheet_detail), detail ? detail : "-");
    s_last_gsheet_tick = xTaskGetTickCount();
}

static bool offline_sync_sanitize_event_queue_locked(void)
{
    size_t write_idx = 0;
    bool changed = false;

    for (size_t read_idx = 0; read_idx < s_event_count; ++read_idx) {
        offline_event_item_t item = s_event_queue[read_idx];
        bool keep = event_type_supported(item.type);

        if (item.type == OFFLINE_EVENT_TG_TEXT) {
            keep = keep && item.a[0] != '\0';
        } else if (item.type == OFFLINE_EVENT_GSHEET) {
            keep = keep && gsheet_event_enabled(item.a);
        }

        if (!keep) {
            changed = true;
            continue;
        }

        if (item.type == OFFLINE_EVENT_GSHEET) {
            bool duplicate_gs = false;
            for (size_t i = 0; i < write_idx; ++i) {
                if (s_event_queue[i].type != OFFLINE_EVENT_GSHEET) continue;
                if (strcmp(s_event_queue[i].a, item.a) == 0 &&
                    strcmp(s_event_queue[i].b, item.b) == 0 &&
                    strcmp(s_event_queue[i].c, item.c) == 0) {
                    duplicate_gs = true;
                    break;
                }
            }
            if (duplicate_gs) {
                changed = true;
                continue;
            }
        }

        if (write_idx > 0) {
            offline_event_item_t *prev = &s_event_queue[write_idx - 1];
            bool duplicate = (prev->type == item.type &&
                              strcmp(prev->a, item.a) == 0 &&
                              strcmp(prev->b, item.b) == 0 &&
                              strcmp(prev->c, item.c) == 0);
            if (duplicate) {
                changed = true;
                continue;
            }
        }

        if (write_idx != read_idx) {
            s_event_queue[write_idx] = item;
            changed = true;
        }
        write_idx++;
    }

    for (size_t i = write_idx; i < OFFLINE_MAX_EVENT_ITEMS; ++i) {
        memset(&s_event_queue[i], 0, sizeof(s_event_queue[i]));
    }

    if (s_event_count != write_idx) {
        s_event_count = write_idx;
        changed = true;
    }

    return changed;
}

// Caller MUST hold offline_sync_lock(). All set_blob+set_u32 calls
// stage on the same handle and commit atomically — partial commits
// would corrupt the queue (e.g., shadow_n=5 but shadow_q only holds 3).
static void offline_sync_save_queues_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(OFFLINE_SYNC_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for offline queue save");
        return;
    }

    esp_err_t e1 = nvs_set_blob(h, "shadow_q", s_shadow_queue, sizeof(s_shadow_queue));
    esp_err_t e2 = nvs_set_u32(h, "shadow_n", (uint32_t)s_shadow_count);
    esp_err_t e3 = nvs_set_blob(h, "event_q", s_event_queue, sizeof(s_event_queue));
    esp_err_t e4 = nvs_set_u32(h, "event_n", (uint32_t)s_event_count);
    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK || e4 != ESP_OK) {
        ESP_LOGW(TAG, "Offline queue NVS set failed (e1=%d e2=%d e3=%d e4=%d)",
                 e1, e2, e3, e4);
    }
    if (nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to commit offline queues to NVS");
    }
    nvs_close(h);
}

static void offline_sync_load_queues_locked(void)
{
    bool sanitized = false;
    nvs_handle_t h;
    if (nvs_open(OFFLINE_SYNC_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        memset(s_shadow_queue, 0, sizeof(s_shadow_queue));
        memset(s_event_queue, 0, sizeof(s_event_queue));
        s_shadow_count = 0;
        s_event_count = 0;
        return;
    }

    size_t blob_len = sizeof(s_shadow_queue);
    uint32_t count = 0;
    if (nvs_get_blob(h, "shadow_q", s_shadow_queue, &blob_len) != ESP_OK || blob_len != sizeof(s_shadow_queue)) {
        memset(s_shadow_queue, 0, sizeof(s_shadow_queue));
    }
    if (nvs_get_u32(h, "shadow_n", &count) == ESP_OK && count <= OFFLINE_MAX_SHADOW_ITEMS) {
        s_shadow_count = count;
    } else {
        s_shadow_count = 0;
    }

    blob_len = sizeof(s_event_queue);
    count = 0;
    if (nvs_get_blob(h, "event_q", s_event_queue, &blob_len) != ESP_OK || blob_len != sizeof(s_event_queue)) {
        memset(s_event_queue, 0, sizeof(s_event_queue));
    }
    if (nvs_get_u32(h, "event_n", &count) == ESP_OK && count <= OFFLINE_MAX_EVENT_ITEMS) {
        s_event_count = count;
    } else {
        s_event_count = 0;
    }

    if (offline_sync_sanitize_event_queue_locked()) {
        ESP_LOGW(TAG, "Sanitized invalid offline event entries");
        sanitized = true;
    }

    nvs_close(h);

    if (sanitized) {
        offline_sync_save_queues_locked();
    }
}

static void shadow_queue_push_locked(const char *payload)
{
    if (!payload || !payload[0]) return;
    if (s_shadow_count >= OFFLINE_MAX_SHADOW_ITEMS) {
        memmove(&s_shadow_queue[0], &s_shadow_queue[1],
                sizeof(s_shadow_queue[0]) * (OFFLINE_MAX_SHADOW_ITEMS - 1));
        s_shadow_count = OFFLINE_MAX_SHADOW_ITEMS - 1;
    }
    memset(&s_shadow_queue[s_shadow_count], 0, sizeof(s_shadow_queue[s_shadow_count]));
    copy_trunc(s_shadow_queue[s_shadow_count].payload,
               sizeof(s_shadow_queue[s_shadow_count].payload), payload);
    s_shadow_count++;
    ESP_LOGI(TAG, "Queued shadow payload for later sync (%u pending)", (unsigned)s_shadow_count);
}

static void event_queue_push_locked(uint8_t type, const char *a, const char *b, const char *c)
{
    if (type == OFFLINE_EVENT_GSHEET) {
        for (size_t i = 0; i < s_event_count; ++i) {
            offline_event_item_t *item = &s_event_queue[i];
            if (item->type != OFFLINE_EVENT_GSHEET) continue;
            if (strcmp(item->a, a ? a : "") == 0 &&
                strcmp(item->b, b ? b : "") == 0 &&
                strcmp(item->c, c ? c : "") == 0) {
                ESP_LOGW(TAG, "Skip duplicate queued GSheet event");
                return;
            }
        }
    }

    if (s_event_count > 0) {
        offline_event_item_t *last = &s_event_queue[s_event_count - 1];
        if (last->type == type &&
            strcmp(last->a, a ? a : "") == 0 &&
            strcmp(last->b, b ? b : "") == 0 &&
            strcmp(last->c, c ? c : "") == 0) {
            ESP_LOGW(TAG, "Skip duplicate offline event type=%u", (unsigned)type);
            return;
        }
    }

    if (s_event_count >= OFFLINE_MAX_EVENT_ITEMS) {
        memmove(&s_event_queue[0], &s_event_queue[1],
                sizeof(s_event_queue[0]) * (OFFLINE_MAX_EVENT_ITEMS - 1));
        s_event_count = OFFLINE_MAX_EVENT_ITEMS - 1;
    }
    memset(&s_event_queue[s_event_count], 0, sizeof(s_event_queue[s_event_count]));
    s_event_queue[s_event_count].type = type;
    copy_trunc(s_event_queue[s_event_count].a, sizeof(s_event_queue[s_event_count].a), a);
    copy_trunc(s_event_queue[s_event_count].b, sizeof(s_event_queue[s_event_count].b), b);
    copy_trunc(s_event_queue[s_event_count].c, sizeof(s_event_queue[s_event_count].c), c);
    s_event_count++;
    ESP_LOGI(TAG, "Queued offline event type=%u (%u pending)", (unsigned)type, (unsigned)s_event_count);
}

static bool send_telegram_text_sync(const char *msg)
{
    const char *bot_token = cloud_secrets_get_telegram_token();
    const char *chat_id = cloud_secrets_get_telegram_chat_id();
    if (!bot_token || !bot_token[0] || !chat_id || !chat_id[0] || !msg || !msg[0]) {
        return false;
    }

    char url[512];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", bot_token);

    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    if (!cJSON_AddStringToObject(root, "chat_id", chat_id) ||
        !cJSON_AddStringToObject(root, "text", msg)) {
        cJSON_Delete(root);
        return false;
    }

    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!post_data) return false;

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(post_data);
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    free(post_data);
    return (err == ESP_OK && status == 200);
}

// Set true once the GSheet endpoint has returned a hard-config error
// (405 Method Not Allowed = deployment likely deleted/redeployed without
// POST permission, 404 = URL changed). After that we stop hammering the
// server every 5 s — events stay queued in NVS so a future reboot with
// a fixed endpoint URL still replays them. Reset on successful send.
static bool s_gsheet_endpoint_dead = false;
static int64_t s_gsheet_dead_log_us = 0;

static bool send_google_sheets_sync(const char *event, const char *meds, const char *detail)
{
    const char *gs_url = cloud_secrets_get_google_script_url();
    if (!gs_url || !gs_url[0]) return false;

    if (s_gsheet_endpoint_dead) {
        // Log a single reminder every 10 min instead of flooding every 5 s.
        int64_t now = esp_timer_get_time();
        if (now - s_gsheet_dead_log_us > 600LL * 1000000LL) {
            s_gsheet_dead_log_us = now;
            ESP_LOGW(TAG, "GSheet endpoint marked dead this session — "
                          "fix the Apps Script deployment URL + reboot to replay queued events");
        }
        return false;
    }

    esp_http_client_config_t config = {
        .url = gs_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;

    cJSON *root = cJSON_CreateObject();
    if (!root ||
        !cJSON_AddStringToObject(root, "event", event ? event : "-") ||
        !cJSON_AddStringToObject(root, "meds", meds ? meds : "-") ||
        !cJSON_AddStringToObject(root, "detail", detail ? detail : "-")) {
        cJSON_Delete(root);
        esp_http_client_cleanup(client);
        return false;
    }

    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!post_data) {
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(post_data);

    // Only accept the send when the HTTP perform actually succeeded AND
    // the server returned a 2xx/3xx status. The previous "status > 0 OR
    // err == ESP_OK" treated any reachable endpoint as success — even
    // 4xx/5xx responses would mark the event as sent and drop it from the
    // retry queue, silently losing dispense events.
    if (err == ESP_OK && gsheet_status_ok(status)) {
        ESP_LOGI(TAG, "GSheet send accepted. Status=%d", status);
        gsheet_mark_recent(event, meds, detail);
        // Endpoint is alive — clear any prior dead flag so future
        // retries resume normally.
        s_gsheet_endpoint_dead = false;
        return true;
    }

    // 404/405 = endpoint mis-configured (URL gone, deployment doesn't
    // accept POST). No amount of retrying will fix this; mark the
    // endpoint dead so we stop spamming the log + network.
    if (err == ESP_OK && (status == 404 || status == 405)) {
        if (!s_gsheet_endpoint_dead) {
            ESP_LOGE(TAG, "GSheet endpoint returned %d — marking dead for this session. "
                          "Verify the Apps Script URL + re-deploy as Web App accepting POST, "
                          "then reboot to flush %u queued events.",
                     status, (unsigned)s_event_count);
            s_gsheet_endpoint_dead = true;
            s_gsheet_dead_log_us = esp_timer_get_time();
        }
        return false;
    }

    ESP_LOGW(TAG, "GSheet send still pending. Status=%d err=%s", status, esp_err_to_name(err));
    return false;
}

static void shadow_queue_pop_locked(void)
{
    if (s_shadow_count == 0) return;
    memmove(&s_shadow_queue[0], &s_shadow_queue[1],
            sizeof(s_shadow_queue[0]) * (s_shadow_count - 1));
    s_shadow_count--;
    memset(&s_shadow_queue[s_shadow_count], 0, sizeof(s_shadow_queue[s_shadow_count]));
}

static void event_queue_remove_locked(size_t index)
{
    if (s_event_count == 0 || index >= s_event_count) return;
    if (index < (s_event_count - 1)) {
        memmove(&s_event_queue[index], &s_event_queue[index + 1],
                sizeof(s_event_queue[0]) * (s_event_count - index - 1));
    }
    s_event_count--;
    memset(&s_event_queue[s_event_count], 0, sizeof(s_event_queue[s_event_count]));
}

static void offline_flush_task(void *arg)
{
    bool shadow_sent = false;

    while (1) {
        offline_shadow_item_t shadow_item = {0};
        offline_event_item_t event_item = {0};
        bool has_shadow = false;
        bool has_event = false;
        bool progressed = false;
        size_t event_index = 0;

        if (!offline_sync_lock()) break;
        if (s_shadow_count > 0) {
            shadow_item = s_shadow_queue[0];
            has_shadow = true;
        }
        if (s_event_count > 0 && wifi_sta_connected()) {
            for (size_t i = 0; i < s_event_count; ++i) {
                offline_event_item_t candidate = s_event_queue[i];
                bool supported = (candidate.type == OFFLINE_EVENT_TG_TEXT ||
                                  candidate.type == OFFLINE_EVENT_GSHEET);
                if (!supported) continue;
                event_item = candidate;
                event_index = i;
                has_event = true;
                break;
            }
        }
        offline_sync_unlock();

        if (has_shadow) {
            if (netpie_is_connected() && netpie_publish_shadow_json(shadow_item.payload)) {
                shadow_sent = true;
                if (offline_sync_lock()) {
                    shadow_queue_pop_locked();
                    offline_sync_save_queues_locked();
                    offline_sync_unlock();
                }
                progressed = true;
                vTaskDelay(pdMS_TO_TICKS(150));
            } else {
                ESP_LOGW(TAG, "Pending shadow sync paused: broker not ready");
            }
        }

        if (has_event) {
            if (wifi_sta_connected()) {
                bool ok = false;
                if (event_item.type == OFFLINE_EVENT_TG_TEXT) {
                    ok = send_telegram_text_sync(event_item.a);
                } else if (event_item.type == OFFLINE_EVENT_GSHEET) {
                    ok = send_google_sheets_sync(event_item.a, event_item.b, event_item.c);
                }
                if (ok) {
                    if (offline_sync_lock()) {
                        event_queue_remove_locked(event_index);
                        offline_sync_save_queues_locked();
                        offline_sync_unlock();
                    }
                    progressed = true;
                    vTaskDelay(pdMS_TO_TICKS(150));
                } else {
                    static int64_t s_event_paused_log_us = 0;
                    int64_t now = esp_timer_get_time();
                    if (now - s_event_paused_log_us > 60LL * 1000000LL) {
                        s_event_paused_log_us = now;
                        ESP_LOGW(TAG, "Pending event sync paused: endpoint not ready (type=%u)",
                                 (unsigned)event_item.type);
                    }
                }
            } else {
                static int64_t s_wifi_paused_log_us = 0;
                int64_t now = esp_timer_get_time();
                if (now - s_wifi_paused_log_us > 60LL * 1000000LL) {
                    s_wifi_paused_log_us = now;
                    ESP_LOGW(TAG, "Pending event sync paused: Wi-Fi not ready");
                }
            }
        }

        if (!progressed) break;
    }

    if (shadow_sent && netpie_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(400));
        netpie_shadow_get();
    }

    s_flush_task = NULL;
    vTaskDelete(NULL);
}

static void offline_watch_task(void *arg)
{
    (void)arg;
    int64_t last_log_us = 0;
    while (1) {
        vTaskDelay(OFFLINE_RETRY_INTERVAL);
        if (!offline_sync_has_pending_work()) continue;
        if (wifi_sta_connected() || netpie_is_connected()) {
            // Rate-limit the "pending offline work" reminder to once
            // per minute so a stuck endpoint doesn't spam the log
            // every 5 s. Flush still runs every cycle — the rate-limit
            // is only on the log line.
            int64_t now = esp_timer_get_time();
            if (now - last_log_us > 60LL * 1000000LL) {
                last_log_us = now;
                ESP_LOGI(TAG, "Background retry: pending offline work detected");
            }
            offline_sync_flush_async();
        }
    }
}

void offline_sync_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_lock) return;

    if (offline_sync_lock()) {
        offline_sync_load_queues_locked();
        offline_sync_unlock();
    }
    if (!s_watch_task) {
        if (xTaskCreate(offline_watch_task, "offline_watch", 4096, NULL, 3, &s_watch_task) != pdPASS) {
            ESP_LOGW(TAG, "Failed to start offline retry watcher");
            s_watch_task = NULL;
        }
    }
}

void offline_sync_queue_shadow_payload(const char *payload)
{
    if (!payload || !payload[0]) return;
    if (!offline_sync_lock()) return;
    shadow_queue_push_locked(payload);
    offline_sync_save_queues_locked();
    offline_sync_unlock();
    if (netpie_is_connected()) offline_sync_flush_async();
}

void offline_sync_queue_telegram_text(const char *msg)
{
    if (!msg || !msg[0]) return;
    if (!offline_sync_lock()) return;
    event_queue_push_locked(OFFLINE_EVENT_TG_TEXT, msg, "", "");
    offline_sync_save_queues_locked();
    offline_sync_unlock();
    if (wifi_sta_connected()) offline_sync_flush_async();
}

void offline_sync_queue_google_sheets(const char *event, const char *meds, const char *detail)
{
    if (!gsheet_event_enabled(event)) return;
    if (gsheet_event_matches_last(event, meds, detail)) return;

    if (wifi_sta_connected()) {
        bool ok = send_google_sheets_sync(event ? event : "-",
                                          meds ? meds : "-",
                                          detail ? detail : "-");
        if (ok) return;
        ESP_LOGW(TAG, "Online Google Sheets send failed; queue for retry");
    }

    if (!offline_sync_lock()) return;
    event_queue_push_locked(OFFLINE_EVENT_GSHEET, event ? event : "-", meds ? meds : "-", detail ? detail : "-");
    offline_sync_save_queues_locked();
    offline_sync_unlock();
}

bool offline_sync_has_pending_work(void)
{
    bool pending = false;
    if (!offline_sync_lock()) return false;
    pending = (s_shadow_count > 0 || s_event_count > 0);
    offline_sync_unlock();
    return pending;
}

size_t offline_sync_pending_count(void)
{
    size_t pending = 0;
    if (!offline_sync_lock()) return 0;
    pending = s_shadow_count + s_event_count;
    offline_sync_unlock();
    return pending;
}

size_t offline_sync_pending_shadow_count(void)
{
    size_t pending = 0;
    if (!offline_sync_lock()) return 0;
    pending = s_shadow_count;
    offline_sync_unlock();
    return pending;
}

size_t offline_sync_pending_event_count(void)
{
    size_t pending = 0;
    if (!offline_sync_lock()) return 0;
    pending = s_event_count;
    offline_sync_unlock();
    return pending;
}

size_t offline_sync_pending_telegram_count(void)
{
    size_t pending = 0;
    if (!offline_sync_lock()) return 0;
    for (size_t i = 0; i < s_event_count; ++i) {
        if (s_event_queue[i].type == OFFLINE_EVENT_TG_TEXT) pending++;
    }
    offline_sync_unlock();
    return pending;
}

size_t offline_sync_pending_gsheet_count(void)
{
    size_t pending = 0;
    if (!offline_sync_lock()) return 0;
    for (size_t i = 0; i < s_event_count; ++i) {
        if (s_event_queue[i].type == OFFLINE_EVENT_GSHEET) pending++;
    }
    offline_sync_unlock();
    return pending;
}

void offline_sync_flush_async(void)
{
    if (s_flush_task || !offline_sync_has_pending_work()) return;
    if (xTaskCreate(offline_flush_task, "offline_flush", 8192, NULL, 4, &s_flush_task) != pdPASS) {
        ESP_LOGW(TAG, "Failed to start offline flush task");
        s_flush_task = NULL;
    }
}
