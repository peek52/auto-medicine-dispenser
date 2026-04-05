#include "offline_sync.h"

#include "cloud_secrets.h"
#include "netpie_mqtt.h"
#include "wifi_sta.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "offline_sync";

#define OFFLINE_SYNC_NAMESPACE "offline_q"
#define OFFLINE_MAX_SHADOW_ITEMS 24
#define OFFLINE_MAX_EVENT_ITEMS  24
#define OFFLINE_SHADOW_PAYLOAD_LEN 192
#define OFFLINE_EVENT_TEXT_LEN   320
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

static void offline_sync_save_queues_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(OFFLINE_SYNC_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for offline queue save");
        return;
    }

    (void)nvs_set_blob(h, "shadow_q", s_shadow_queue, sizeof(s_shadow_queue));
    (void)nvs_set_u32(h, "shadow_n", (uint32_t)s_shadow_count);
    (void)nvs_set_blob(h, "event_q", s_event_queue, sizeof(s_event_queue));
    (void)nvs_set_u32(h, "event_n", (uint32_t)s_event_count);
    if (nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to commit offline queues to NVS");
    }
    nvs_close(h);
}

static void offline_sync_load_queues_locked(void)
{
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

    nvs_close(h);
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

static bool send_google_sheets_sync(const char *event, const char *meds, const char *detail)
{
    const char *gs_url = cloud_secrets_get_google_script_url();
    if (!gs_url || !gs_url[0]) return false;

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
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    free(post_data);
    return (err == ESP_OK && status >= 200 && status < 300);
}

static void shadow_queue_pop_locked(void)
{
    if (s_shadow_count == 0) return;
    memmove(&s_shadow_queue[0], &s_shadow_queue[1],
            sizeof(s_shadow_queue[0]) * (s_shadow_count - 1));
    s_shadow_count--;
    memset(&s_shadow_queue[s_shadow_count], 0, sizeof(s_shadow_queue[s_shadow_count]));
}

static void event_queue_pop_locked(void)
{
    if (s_event_count == 0) return;
    memmove(&s_event_queue[0], &s_event_queue[1],
            sizeof(s_event_queue[0]) * (s_event_count - 1));
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

        if (!offline_sync_lock()) break;
        if (s_shadow_count > 0) {
            shadow_item = s_shadow_queue[0];
            has_shadow = true;
        } else if (s_event_count > 0) {
            event_item = s_event_queue[0];
            has_event = true;
        }
        offline_sync_unlock();

        if (has_shadow) {
            if (!netpie_is_connected() || !netpie_publish_shadow_json(shadow_item.payload)) {
                ESP_LOGW(TAG, "Pending shadow sync paused: broker not ready");
                break;
            }
            shadow_sent = true;
            if (offline_sync_lock()) {
                shadow_queue_pop_locked();
                offline_sync_save_queues_locked();
                offline_sync_unlock();
            }
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }

        if (has_event) {
            if (!wifi_sta_connected()) break;

            bool ok = false;
            if (event_item.type == OFFLINE_EVENT_TG_TEXT) {
                ok = send_telegram_text_sync(event_item.a);
            } else if (event_item.type == OFFLINE_EVENT_GSHEET) {
                ok = send_google_sheets_sync(event_item.a, event_item.b, event_item.c);
            }
            if (!ok) break;

            if (offline_sync_lock()) {
                event_queue_pop_locked();
                offline_sync_save_queues_locked();
                offline_sync_unlock();
            }
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }

        break;
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
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        if (!offline_sync_has_pending_work()) continue;
        if (wifi_sta_connected() || netpie_is_connected()) {
            ESP_LOGI(TAG, "Background retry: pending offline work detected");
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
    if (!offline_sync_lock()) return;
    event_queue_push_locked(OFFLINE_EVENT_GSHEET, event ? event : "-", meds ? meds : "-", detail ? detail : "-");
    offline_sync_save_queues_locked();
    offline_sync_unlock();
    if (wifi_sta_connected()) offline_sync_flush_async();
}

bool offline_sync_has_pending_work(void)
{
    bool pending = false;
    if (!offline_sync_lock()) return false;
    pending = (s_shadow_count > 0 || s_event_count > 0);
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
