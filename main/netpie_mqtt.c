#include "netpie_mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "offline_sync.h"

static const char *TAG = "netpie";
static const char *SHADOW_CACHE_NAMESPACE = "shadow_cache";
static const char *SHADOW_CACHE_KEY = "shadow_blob";

static esp_mqtt_client_handle_t s_client = NULL;
static bool s_connected = false;
// Working shadow — only mutated under shadow_lock(). Readers that need
// a multi-field consistent snapshot must use netpie_shadow_copy() OR
// dereference netpie_get_shadow() which returns the published double
// buffer below.
static netpie_shadow_t s_shadow = {0};
// Published snapshot — atomically swapped at the end of parse_shadow.
// Readers see a stable struct as long as they keep their pointer.
static netpie_shadow_t s_shadow_pub_a = {0};
static netpie_shadow_t s_shadow_pub_b = {0};
static netpie_shadow_t * volatile s_shadow_pub = &s_shadow_pub_a;
static SemaphoreHandle_t s_mutex = NULL;
// Atomic on RISC-V (32-bit aligned). Read with portMUX-free volatile
// access — readers only need a coarse "seconds since RX" check.
static volatile uint32_t s_last_rx_ticks = 0;

// Publish the current s_shadow to the inactive snapshot buffer and
// flip the pointer. Caller MUST hold shadow_lock(). This is the only
// place s_shadow_pub_{a,b} are written, so no reader/writer races on
// the buffers themselves — readers always see a complete snapshot.
static void shadow_publish_locked(void)
{
    netpie_shadow_t *next = (s_shadow_pub == &s_shadow_pub_a)
                              ? &s_shadow_pub_b : &s_shadow_pub_a;
    *next = s_shadow;
    s_shadow_pub = next;  // 32-bit aligned pointer = atomic swap
}

static const char *s_slot_keys[7] = {
    "t_morn_pre", "t_morn_post",
    "t_noon_pre", "t_noon_post",
    "t_eve_pre", "t_eve_post",
    "t_bed"
};

uint32_t netpie_mqtt_get_last_rx_time(void)
{
    return s_last_rx_ticks;
}

static bool ensure_shadow_mutex(void)
{
    if (s_mutex) return true;
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex != NULL;
}

static bool shadow_lock(void)
{
    return ensure_shadow_mutex() && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE;
}

static void shadow_unlock(void)
{
    // Publish a fresh snapshot every time we release the lock so any
    // caller of netpie_get_shadow() sees a fully-formed shadow even if
    // the next parse_shadow starts immediately. ~500 bytes memcpy is
    // cheap relative to the tens-of-ms a parse takes anyway.
    if (s_mutex) {
        shadow_publish_locked();
        xSemaphoreGive(s_mutex);
    }
}

static uint8_t normalize_med_slots_mask(uint8_t slots_mask)
{
    slots_mask &= 0x7F;
    for (int base = 0; base <= 4; base += 2) {
        uint8_t pair_mask = (uint8_t)((1U << base) | (1U << (base + 1)));
        if ((slots_mask & pair_mask) == pair_mask) {
            slots_mask &= (uint8_t)~(1U << (base + 1));
        }
    }
    return slots_mask;
}

static void shadow_set_defaults_locked(void)
{
    const char *defaults[7] = {
        "08:00", "08:30", "12:00", "12:30", "17:00", "17:30", "21:00"
    };

    memset(&s_shadow, 0, sizeof(s_shadow));
    s_shadow.enabled = true;
    s_shadow.max_pills = DISPENSER_MAX_PILLS;    /* default ceiling */
    s_shadow.loaded = true;
    for (int i = 0; i < 7; i++) {
        strlcpy(s_shadow.slot_time[i], defaults[i], sizeof(s_shadow.slot_time[i]));
    }
}

int dispenser_max_pills(void)
{
    /* Shadow load happens early in boot; if a caller hits this before
     * load_default_shadow runs or before the NVS cache is restored,
     * s_shadow.max_pills could be 0 → fall back to the compile-time
     * constant rather than clamping every count to 0. */
    int v = s_shadow.max_pills;
    if (v <= 0 || v > 999) return DISPENSER_MAX_PILLS;
    return v;
}

static void shadow_cache_save_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(SHADOW_CACHE_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_blob(h, SHADOW_CACHE_KEY, &s_shadow, sizeof(s_shadow));
    (void)nvs_commit(h);
    nvs_close(h);
}

static bool shadow_cache_load_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(SHADOW_CACHE_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    netpie_shadow_t cached = {0};
    size_t blob_len = sizeof(cached);
    bool ok = nvs_get_blob(h, SHADOW_CACHE_KEY, &cached, &blob_len) == ESP_OK &&
              blob_len == sizeof(cached);
    nvs_close(h);
    if (!ok) return false;

    s_shadow = cached;
    s_shadow.loaded = true;
    shadow_publish_locked();
    return true;
}

static char *build_shadow_payload_int(const char *key, int value)
{
    if (!key) return NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (!root || !data) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return NULL;
    }
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddNumberToObject(data, key, value);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

static char *build_shadow_payload_string(const char *key, const char *value)
{
    if (!key || !value) return NULL;
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (!root || !data) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return NULL;
    }
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddStringToObject(data, key, value);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

/* Publish-inhibit counter — when > 0, publish_shadow_payload drops the
 * outgoing MQTT message but local shadow + NVS still get updated.
 * Used by the meds-detail UI to batch +/- / slot / rename edits and
 * commit only once the user presses Save or Back, instead of flooding
 * NETPIE on every tap. Counter is nest-safe via push/pop. */
static volatile int s_publish_inhibit_count = 0;

void netpie_publish_inhibit_push(void)
{
    s_publish_inhibit_count++;
}

void netpie_publish_inhibit_pop(void)
{
    if (s_publish_inhibit_count > 0) s_publish_inhibit_count--;
}

static void publish_shadow_payload(char *payload)
{
    if (!payload) return;
    if (s_publish_inhibit_count > 0) {
        /* Edit-in-progress; caller will explicitly republish on commit. */
        free(payload);
        return;
    }
    if (s_client && s_connected) {
        esp_mqtt_client_publish(s_client, NETPIE_TOPIC_SET, payload, 0, 0, 0);
        ESP_LOGI(TAG, "Shadow updated: %s", payload);
    } else {
        ESP_LOGI(TAG, "Shadow queued while offline: %s", payload);
        offline_sync_queue_shadow_payload(payload);
    }
    free(payload);
}

void netpie_shadow_commit_med_diff(int med_id, const netpie_med_t *backup)
{
    if (!backup || med_id < 1 || med_id > DISPENSER_MED_COUNT) return;
    const netpie_med_t *cur = &netpie_get_shadow()->med[med_id - 1];

    if (strcmp(cur->name, backup->name) != 0) {
        char key[24];
        snprintf(key, sizeof(key), "med%d_name", med_id);
        publish_shadow_payload(build_shadow_payload_string(key, cur->name));
    }
    if (cur->count != backup->count) {
        char key[24];
        snprintf(key, sizeof(key), "med%d_count", med_id);
        publish_shadow_payload(build_shadow_payload_int(key, cur->count));
    }
    if (cur->slots != backup->slots) {
        char key[24];
        snprintf(key, sizeof(key), "med%d_slots", med_id);
        publish_shadow_payload(build_shadow_payload_int(key, cur->slots));
    }
}

static bool json_get_str(const char *json, const char *key, char *buf_out, size_t buf_len)
{
    if (!json || !key || !buf_out || buf_len == 0) return false;

    char search[48];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;

    size_t i = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && i < buf_len - 1) {
            if (*p == '\\' && p[1]) p++;
            buf_out[i++] = *p++;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && *p != '\r' && *p != '\n' && i < buf_len - 1) {
            buf_out[i++] = *p++;
        }
        while (i > 0 && (buf_out[i - 1] == ' ' || buf_out[i - 1] == '\t')) i--;
    }
    buf_out[i] = '\0';
    return i > 0;
}

static void parse_shadow(const char *json)
{
    if (!json) return;
    const char *data = strstr(json, "\"data\":");
    if (!data) data = json;

    if (!shadow_lock()) return;

    char tmp[32];
    if (json_get_str(data, "scheduleEnabled", tmp, sizeof(tmp))) {
        s_shadow.enabled = atoi(tmp) == 1 || strcmp(tmp, "true") == 0;
    }

    /* Per-module pill ceiling from the web/touch UI. Clamp to 1..999 so
     * a stale / corrupted shadow value doesn't lock the dispenser into
     * 0 or wrap-around state. */
    if (json_get_str(data, "max_pills", tmp, sizeof(tmp))) {
        int mp = atoi(tmp);
        if (mp < 1)   mp = DISPENSER_MAX_PILLS;
        if (mp > 999) mp = 999;
        s_shadow.max_pills = mp;
    }

    for (int i = 0; i < 7; i++) {
        char buf[16];
        if (json_get_str(data, s_slot_keys[i], buf, sizeof(buf))) {
            strlcpy(s_shadow.slot_time[i], buf, sizeof(s_shadow.slot_time[i]));
        }
    }

    for (int i = 0; i < DISPENSER_MED_COUNT; i++) {
        char key[24];
        char val_buf[40];
        int id = i + 1;

        snprintf(key, sizeof(key), "med%d_name", id);
        if (json_get_str(data, key, val_buf, sizeof(val_buf))) {
            strlcpy(s_shadow.med[i].name, val_buf, sizeof(s_shadow.med[i].name));
        }

        snprintf(key, sizeof(key), "med%d_count", id);
        if (json_get_str(data, key, val_buf, sizeof(val_buf))) {
            int cnt = atoi(val_buf);
            int cap = dispenser_max_pills();
            if (cnt < 0)   cnt = 0;
            if (cnt > cap) cnt = cap;
            s_shadow.med[i].count = cnt;
        }

        snprintf(key, sizeof(key), "med%d_slots", id);
        if (json_get_str(data, key, val_buf, sizeof(val_buf))) {
            s_shadow.med[i].slots = normalize_med_slots_mask((uint8_t)atoi(val_buf));
        }
    }

    char disp_buf[24];
    if (json_get_str(data, "disp_req", disp_buf, sizeof(disp_buf)) &&
        disp_buf[0] && strcmp(disp_buf, "0,0") != 0 && strcmp(disp_buf, "0") != 0) {
        int mod_id = 0;
        int qty = 0;
        if (sscanf(disp_buf, "%d,%d", &mod_id, &qty) == 2 &&
            mod_id >= 1 && mod_id <= DISPENSER_MED_COUNT && qty > 0) {
            ESP_LOGI(TAG, "NETPIE remote dispense request: module=%d qty=%d", mod_id, qty);
            extern void dispenser_manual_dispense(int med_idx, int qty);
            dispenser_manual_dispense(mod_id - 1, qty);
            if (s_client) {
                esp_mqtt_client_publish(s_client, NETPIE_TOPIC_SET,
                                        "{\"data\":{\"disp_req\":\"0,0\"}}", 0, 0, 0);
            }
        }
    }

    s_shadow.loaded = true;
    s_last_rx_ticks = xTaskGetTickCount();
    shadow_cache_save_locked();
    // shadow_unlock() publishes the snapshot atomically.
    shadow_unlock();
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            ESP_LOGI(TAG, "Connected to NETPIE broker");
            esp_mqtt_client_subscribe(s_client, NETPIE_TOPIC_RESP, 0);
            esp_mqtt_client_subscribe(s_client, NETPIE_TOPIC_UPDATED, 0);
            netpie_shadow_get();
            offline_sync_flush_async();
            break;

        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "Disconnected from NETPIE");
            break;

        case MQTT_EVENT_DATA:
            if (ev && ev->topic_len > 0 && ev->data_len > 0) {
                char topic[80] = {0};
                int topic_len = ev->topic_len < (int)sizeof(topic) - 1 ? ev->topic_len : (int)sizeof(topic) - 1;
                memcpy(topic, ev->topic, topic_len);

                if (strstr(topic, "@shadow/data/get/response") || strstr(topic, "@shadow/data/updated")) {
                    char *buf = (char *)malloc(ev->data_len + 1);
                    if (buf) {
                        memcpy(buf, ev->data, ev->data_len);
                        buf[ev->data_len] = '\0';
                        parse_shadow(buf);
                        free(buf);
                    }
                }
            }
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "MQTT error");
            break;

        default:
            break;
    }
}

void netpie_init(void)
{
    if (!shadow_lock()) {
        ESP_LOGE(TAG, "Failed to create shadow mutex");
        return;
    }
    if (!shadow_cache_load_locked()) {
        shadow_set_defaults_locked();
        shadow_cache_save_locked();
    }
    shadow_unlock();

    esp_mqtt_client_config_t cfg = {0};
    cfg.broker.address.hostname = NETPIE_BROKER;
    cfg.broker.address.port = NETPIE_PORT;
    cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
    cfg.credentials.client_id = NETPIE_CLIENT_ID;
    cfg.credentials.username = NETPIE_TOKEN;
    cfg.credentials.authentication.password = NETPIE_SECRET;
    cfg.network.reconnect_timeout_ms = 5000;
    cfg.network.timeout_ms = 10000;
    cfg.session.keepalive = 60;

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        return;
    }
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "MQTT client started: %s:%d", NETPIE_BROKER, NETPIE_PORT);
}

void netpie_shadow_get(void)
{
    if (!s_client) return;
    esp_mqtt_client_publish(s_client, NETPIE_TOPIC_GET, "{}", 2, 0, 0);
}

void netpie_shadow_update_count(int med_id, int new_count)
{
    if (med_id < 1 || med_id > DISPENSER_MED_COUNT) return;
    if (new_count < 0) new_count = 0;
    int cap = dispenser_max_pills();
    if (new_count > cap) new_count = cap;

    if (shadow_lock()) {
        s_shadow.med[med_id - 1].count = new_count;
        shadow_cache_save_locked();
        shadow_unlock();
    }

    char key[24];
    snprintf(key, sizeof(key), "med%d_count", med_id);
    publish_shadow_payload(build_shadow_payload_int(key, new_count));
}

void netpie_shadow_update_med_name(int med_id, const char *name)
{
    if (!name || med_id < 1 || med_id > DISPENSER_MED_COUNT) return;

    char safe_name[32];
    strlcpy(safe_name, name, sizeof(safe_name));

    // Trim leading + trailing whitespace so "Para " and "Para" are stored
    // identically (otherwise matching/sort breaks downstream).
    char *start = safe_name;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != safe_name) memmove(safe_name, start, strlen(start) + 1);
    size_t len = strlen(safe_name);
    while (len > 0 && isspace((unsigned char)safe_name[len - 1])) {
        safe_name[--len] = '\0';
    }

    if (shadow_lock()) {
        strlcpy(s_shadow.med[med_id - 1].name, safe_name, sizeof(s_shadow.med[med_id - 1].name));
        shadow_cache_save_locked();
        shadow_unlock();
    }

    char key[24];
    snprintf(key, sizeof(key), "med%d_name", med_id);
    publish_shadow_payload(build_shadow_payload_string(key, safe_name));
}

void netpie_shadow_update_med_slots(int med_id, uint8_t slots_mask)
{
    if (med_id < 1 || med_id > DISPENSER_MED_COUNT) return;
    slots_mask = normalize_med_slots_mask(slots_mask);

    if (shadow_lock()) {
        s_shadow.med[med_id - 1].slots = slots_mask;
        shadow_cache_save_locked();
        shadow_unlock();
    }

    char key[24];
    snprintf(key, sizeof(key), "med%d_slots", med_id);
    publish_shadow_payload(build_shadow_payload_int(key, slots_mask));
}

void netpie_shadow_update_slot(int slot_idx, const char *hh_mm)
{
    if (!hh_mm || slot_idx < 0 || slot_idx >= 7) return;

    // HH:MM validation — reject "25:00", "12:60", "ab:cd", "1:2",
    // "10:5", etc. Strict 5-char format with two digits, colon, two
    // digits, in valid 24-hour ranges. Empty string ("--:--") is also
    // valid since it means "slot disabled".
    char safe_time[6] = {0};
    if (strcmp(hh_mm, "--:--") == 0 || hh_mm[0] == '\0') {
        strlcpy(safe_time, "--:--", sizeof(safe_time));
    } else {
        if (strlen(hh_mm) != 5 || hh_mm[2] != ':' ||
            !isdigit((unsigned char)hh_mm[0]) || !isdigit((unsigned char)hh_mm[1]) ||
            !isdigit((unsigned char)hh_mm[3]) || !isdigit((unsigned char)hh_mm[4])) {
            return;  // malformed
        }
        int hh = (hh_mm[0]-'0')*10 + (hh_mm[1]-'0');
        int mm = (hh_mm[3]-'0')*10 + (hh_mm[4]-'0');
        if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return;  // out of range
        strlcpy(safe_time, hh_mm, sizeof(safe_time));
    }

    if (shadow_lock()) {
        strlcpy(s_shadow.slot_time[slot_idx], safe_time, sizeof(s_shadow.slot_time[slot_idx]));
        shadow_cache_save_locked();
        shadow_unlock();
    }

    publish_shadow_payload(build_shadow_payload_string(s_slot_keys[slot_idx], safe_time));
}

void netpie_shadow_update_enabled(bool enabled)
{
    if (shadow_lock()) {
        s_shadow.enabled = enabled;
        shadow_cache_save_locked();
        shadow_unlock();
    }
    publish_shadow_payload(build_shadow_payload_int("scheduleEnabled", enabled ? 1 : 0));
}

bool netpie_shadow_copy(netpie_shadow_t *out_shadow)
{
    if (!out_shadow) return false;
    if (!shadow_lock()) return false;
    *out_shadow = s_shadow;
    shadow_unlock();
    return true;
}

const netpie_shadow_t *netpie_get_shadow(void)
{
    // Returns the most recently published snapshot. The pointer is
    // stable for the caller's read sequence even if a parse_shadow
    // arrives mid-read — the parse will publish to the OTHER buffer
    // and flip the pointer atomically after the caller is done.
    // Use netpie_shadow_copy() for callers that must serialize with
    // a parse in flight.
    return s_shadow_pub;
}

bool netpie_is_connected(void)
{
    return s_connected;
}

bool netpie_publish_shadow_json(const char *payload)
{
    if (!payload || !payload[0] || !s_client || !s_connected) return false;
    int msg_id = esp_mqtt_client_publish(s_client, NETPIE_TOPIC_SET, payload, 0, 0, 0);
    if (msg_id < 0) return false;
    ESP_LOGI(TAG, "Replayed pending shadow payload: %s", payload);
    return true;
}
