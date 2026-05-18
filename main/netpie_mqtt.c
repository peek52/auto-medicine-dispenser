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
#include "telegram_bot.h"

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

/* Pending-approval buffer. See netpie_mqtt.h. Protected by s_mutex —
 * same lock that guards s_shadow since reads/writes are interleaved. */
static netpie_pending_t s_pending = {0};

/* False until the first MQTT shadow message is parsed after netpie_init.
 * That first message is the device-side "pull" from cloud (the answer to
 * netpie_shadow_get) and is applied directly without prompting. Every
 * subsequent message goes through the pending-approval flow if it
 * differs from the local shadow. */
static bool s_shadow_synced_once = false;

/* One-shot notice flag. Set in parse_shadow on the inactive→active
 * transition, consumed by the Telegram poll task once it has dispatched
 * the "pending approval" announcement. Volatile because reader and
 * writer live on different tasks. */
static volatile bool s_pending_notify_telegram = false;
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

/* Parse "HH:MM" → minutes since midnight. Returns -1 for empty/disabled
 * slot ("--:--" or ""), or -2 for malformed input. */
static int slot_hhmm_minutes(const char *s)
{
    if (!s || s[0] == '\0' || strcmp(s, "--:--") == 0) return -1;
    if (strlen(s) != 5 || s[2] != ':' ||
        !isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1]) ||
        !isdigit((unsigned char)s[3]) || !isdigit((unsigned char)s[4])) return -2;
    int h = (s[0]-'0')*10 + (s[1]-'0');
    int m = (s[3]-'0')*10 + (s[4]-'0');
    if (h < 0 || h > 23 || m < 0 || m > 59) return -2;
    return h*60 + m;
}

/* Returns true iff the slot_time[] set satisfies the user's rules
 * (spec 2026-05-15):
 *   1. Within each meal pair (morn, noon, eve): pre < post.
 *   2. No two set slots share the same HH:MM ("ห้ามตั้งเวลาเดียวกัน").
 * Empty slots are ignored (a slot at "--:--" is disabled). Cross-meal
 * ordering between distinct pairs is NOT enforced; operators are free
 * to set, e.g., bed before morn if their day runs that way.
 * Caller passes a (slot_idx, proposed) pair representing the candidate
 * write; the rest comes from `times`. Pass slot_idx=-1 + proposed=NULL
 * to validate an already-merged array. */
static bool slot_times_monotonic(const char *times[7], int slot_idx, const char *proposed)
{
    int mins[7];
    for (int i = 0; i < 7; i++) {
        const char *s = (i == slot_idx) ? proposed : times[i];
        int v = slot_hhmm_minutes(s);
        if (v == -2) return false;   /* malformed → reject */
        mins[i] = v;
    }
    /* Rule 1: pre < post within each meal pair AND gap ≤ 60 min.
     * Pair indices: (0,1) morn, (2,3) noon, (4,5) eve. `bed` (6) is
     * standalone — no pre/post counterpart to check.
     * The 60-min cap stops operators from setting clinically nonsensical
     * before↔after spreads (user spec 2026-05-18: "ต้องไม่เกิน 1ชม.")
     * and keeps the after-meal alarm from drifting so far that the
     * before-meal 15-min timeout + skip path no longer protects it. */
    const int pairs[3][2] = { {0, 1}, {2, 3}, {4, 5} };
    for (int p = 0; p < 3; p++) {
        int pre  = mins[pairs[p][0]];
        int post = mins[pairs[p][1]];
        if (pre < 0 || post < 0) continue;
        if (post <= pre) return false;
        if (post - pre > 60) return false;
    }
    /* Rule 2: no duplicate HH:MM across any set slots. */
    for (int i = 0; i < 7; i++) {
        if (mins[i] < 0) continue;
        for (int j = i + 1; j < 7; j++) {
            if (mins[j] < 0) continue;
            if (mins[i] == mins[j]) return false;
        }
    }
    return true;
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

/* `is_initial_response` MUST be true only for messages arriving on
 * @shadow/data/get/response — that topic is the device-initiated pull
 * from cloud at MQTT connect, so the first such message is treated as
 * the authoritative sync and applied silently.
 *
 * Anything on @shadow/data/updated is treated as a third-party write
 * (web widget, another client, our own publish echo) — never a silent
 * sync. This closes the bug where a /updated message arriving before
 * the GET response could silently absorb a web-widget save. */
static void parse_shadow(const char *json, bool is_initial_response)
{
    if (!json) return;
    const char *data = strstr(json, "\"data\":");
    if (!data) data = json;

    if (!shadow_lock()) return;

    /* Only the FIRST GET response gets the silent-apply path; all other
     * messages (including subsequent responses on reconnect and every
     * /updated message) go through the diff/pending-approval flow. */
    bool first_sync = is_initial_response && !s_shadow_synced_once;

    /* Start the incoming candidate as a copy of the current shadow so
     * non-mentioned fields stay untouched, then patch in only the
     * fields that appear in the JSON. */
    netpie_shadow_t incoming = s_shadow;
    bool any_diff = false;
    bool enabled_diff = false;
    bool max_pills_diff = false;
    bool slot_time_diff[7] = {0};
    bool med_name_diff[DISPENSER_MED_COUNT] = {0};
    bool med_count_diff[DISPENSER_MED_COUNT] = {0};
    bool med_slots_diff[DISPENSER_MED_COUNT] = {0};

    char tmp[32];
    if (json_get_str(data, "scheduleEnabled", tmp, sizeof(tmp))) {
        bool new_enabled = (atoi(tmp) == 1 || strcmp(tmp, "true") == 0);
        if (new_enabled != s_shadow.enabled) {
            incoming.enabled = new_enabled;
            enabled_diff = true;
            any_diff = true;
        }
    }

    /* Per-module pill ceiling from the web/touch UI. Clamp to 1..999 so
     * a stale / corrupted shadow value doesn't lock the dispenser into
     * 0 or wrap-around state. Note: `incoming.max_pills` becomes the
     * effective cap for clamping the med%d_count fields below — without
     * that, a synchronous "raise max_pills + raise count" save would
     * see the count clamped against the OLD cap and lose user input. */
    int effective_cap = s_shadow.max_pills;
    if (effective_cap <= 0 || effective_cap > 999) effective_cap = DISPENSER_MAX_PILLS;
    if (json_get_str(data, "max_pills", tmp, sizeof(tmp))) {
        int mp = atoi(tmp);
        if (mp < 1)   mp = DISPENSER_MAX_PILLS;
        if (mp > 999) mp = 999;
        if (mp != s_shadow.max_pills) {
            incoming.max_pills = mp;
            max_pills_diff = true;
            any_diff = true;
        }
        /* Use the new cap for clamp even when only the cap field is in
         * this single payload — both raising and lowering must take
         * effect on the count fields in the same message. */
        effective_cap = mp;
    }

    for (int i = 0; i < 7; i++) {
        char buf[16];
        if (json_get_str(data, s_slot_keys[i], buf, sizeof(buf))) {
            if (strcmp(buf, s_shadow.slot_time[i]) != 0) {
                strlcpy(incoming.slot_time[i], buf, sizeof(incoming.slot_time[i]));
                slot_time_diff[i] = true;
                any_diff = true;
            }
        }
    }

    /* Enforce monotonic slot ordering on the candidate set. If a NETPIE
     * client tries to set, e.g., morn_post < morn_pre or noon_pre <
     * morn_post, revert the slot_time portion of the diff (keep med
     * and enable diffs). Logged so the operator can see the rejection
     * in the serial monitor. */
    {
        const char *cand[7];
        for (int i = 0; i < 7; i++) cand[i] = incoming.slot_time[i];
        if (!slot_times_monotonic(cand, -1, NULL)) {
            ESP_LOGW(TAG, "Rejected NETPIE slot_time set — non-monotonic order");
            for (int i = 0; i < 7; i++) {
                if (slot_time_diff[i]) {
                    strlcpy(incoming.slot_time[i], s_shadow.slot_time[i],
                            sizeof(incoming.slot_time[i]));
                    slot_time_diff[i] = false;
                }
            }
            /* Re-compute any_diff: keep true only if at least one non-
             * slot_time field still differs. Simplest: walk the other
             * diff flags. */
            any_diff = enabled_diff || max_pills_diff;
            for (int i = 0; i < DISPENSER_MED_COUNT && !any_diff; i++) {
                if (med_name_diff[i] || med_count_diff[i] || med_slots_diff[i]) {
                    any_diff = true;
                }
            }
        }
    }

    for (int i = 0; i < DISPENSER_MED_COUNT; i++) {
        char key[24];
        char val_buf[40];
        int id = i + 1;

        snprintf(key, sizeof(key), "med%d_name", id);
        if (json_get_str(data, key, val_buf, sizeof(val_buf))) {
            if (strcmp(val_buf, s_shadow.med[i].name) != 0) {
                strlcpy(incoming.med[i].name, val_buf, sizeof(incoming.med[i].name));
                med_name_diff[i] = true;
                any_diff = true;
            }
        }

        snprintf(key, sizeof(key), "med%d_count", id);
        if (json_get_str(data, key, val_buf, sizeof(val_buf))) {
            int cnt = atoi(val_buf);
            if (cnt < 0)            cnt = 0;
            if (cnt > effective_cap) cnt = effective_cap;
            if (cnt != s_shadow.med[i].count) {
                incoming.med[i].count = cnt;
                med_count_diff[i] = true;
                any_diff = true;
            }
        }

        snprintf(key, sizeof(key), "med%d_slots", id);
        if (json_get_str(data, key, val_buf, sizeof(val_buf))) {
            uint8_t sl = normalize_med_slots_mask((uint8_t)atoi(val_buf));
            if (sl != s_shadow.med[i].slots) {
                incoming.med[i].slots = sl;
                med_slots_diff[i] = true;
                any_diff = true;
            }
        }
    }

    /* disp_req is a transient command, not a stored config value —
     * always handle it live, never via the pending-approval buffer.
     *
     * Decode under the lock but defer the actual side effects
     * (dispenser_clear_all_start / dispenser_manual_dispense / MQTT
     * publish) until AFTER shadow_unlock(). Holding shadow_lock across
     * those would risk a future deadlock if any of them ever ended up
     * re-entering the shadow lock. */
    enum { DISP_NONE, DISP_MANUAL, DISP_CLEAR_ALL } disp_cmd = DISP_NONE;
    int  disp_mod_id = 0;
    int  disp_qty    = 0;
    char disp_buf[24];
    if (json_get_str(data, "disp_req", disp_buf, sizeof(disp_buf)) &&
        disp_buf[0] && strcmp(disp_buf, "0,0") != 0 && strcmp(disp_buf, "0") != 0) {
        ESP_LOGI(TAG, "disp_req received: '%s'", disp_buf);
        /* Accept multiple aliases for the system-wide clear command.
         * User report: NETPIE widget "ล้างยาทุกตลับ" (sends "all") was
         * being silently ignored on some firmware builds while per-
         * module dispense (sends "M,N") worked. Adding "clear" and
         * "clearall" tokens future-proofs the parser against widget
         * code churn and makes the command robust to case folding. */
        if (strcmp(disp_buf, "all") == 0 ||
            strcmp(disp_buf, "ALL") == 0 ||
            strcmp(disp_buf, "clear") == 0 ||
            strcmp(disp_buf, "clearall") == 0 ||
            strcmp(disp_buf, "clear_all") == 0) {
            disp_cmd = DISP_CLEAR_ALL;
        } else if (sscanf(disp_buf, "%d,%d", &disp_mod_id, &disp_qty) == 2 &&
                   disp_mod_id >= 1 && disp_mod_id <= DISPENSER_MED_COUNT &&
                   disp_qty > 0) {
            disp_cmd = DISP_MANUAL;
        } else {
            ESP_LOGW(TAG, "disp_req '%s' did not match any known command", disp_buf);
        }
    }

    if (first_sync) {
        /* First cloud sync — apply silently. */
        s_shadow = incoming;
        s_shadow_synced_once = true;
        shadow_cache_save_locked();
    } else if (any_diff) {
        /* External write detected (web widget). Buffer in s_pending and
         * leave s_shadow untouched; the touch UI + Telegram will prompt
         * for approval. If a pending review is already active when a
         * newer write arrives, latest values win — operator approves
         * (or rejects) the latest state. Reset arrived_tick only on
         * the first transition into active so timeouts (if ever added)
         * track when the operator was first asked. */
        bool was_active = s_pending.active;
        s_pending.active = true;
        s_pending.snapshot = incoming;
        s_pending.enabled_diff = enabled_diff;
        s_pending.max_pills_diff = max_pills_diff;
        memcpy(s_pending.slot_time_diff, slot_time_diff, sizeof(slot_time_diff));
        memcpy(s_pending.med_name_diff, med_name_diff, sizeof(med_name_diff));
        memcpy(s_pending.med_count_diff, med_count_diff, sizeof(med_count_diff));
        memcpy(s_pending.med_slots_diff, med_slots_diff, sizeof(med_slots_diff));
        if (!was_active) {
            s_pending.arrived_tick = xTaskGetTickCount();
        }
        /* Re-arm the Telegram notify flag on EVERY external diff, not
         * only the inactive→active transition. A second web write
         * arriving before the operator replies overwrites the snapshot
         * — without re-arming, the user would /approve based on a
         * stale message and inadvertently commit values they never saw. */
        s_pending_notify_telegram = true;
        ESP_LOGI(TAG, "NETPIE pending shadow update — awaiting approval");
    }
    /* else: self-echo (all incoming fields match current shadow) →
     * silently drop. Touch UI / web /tech writes already updated
     * s_shadow before publishing, so the cloud echo finds zero diff. */

    s_shadow.loaded = true;
    s_last_rx_ticks = xTaskGetTickCount();
    // shadow_unlock() publishes the snapshot atomically.
    shadow_unlock();

    /* Execute the disp_req side effects AFTER releasing the shadow lock
     * — see the decode block above. */
    if (disp_cmd == DISP_CLEAR_ALL) {
        ESP_LOGI(TAG, "NETPIE remote clear-all request");
        extern bool dispenser_clear_all_start(void);
        extern bool dispenser_clear_all_active(void);
        bool started = dispenser_clear_all_start();
        if (!started) {
            ESP_LOGW(TAG, "Clear-all start rejected (already running=%d). "
                          "Caller may have left a stale state — exposing to the user.",
                     (int)dispenser_clear_all_active());
        }
    } else if (disp_cmd == DISP_MANUAL) {
        ESP_LOGI(TAG, "NETPIE remote dispense request: module=%d qty=%d",
                 disp_mod_id, disp_qty);
        extern void dispenser_manual_dispense(int med_idx, int qty);
        dispenser_manual_dispense(disp_mod_id - 1, disp_qty);
    }
    if (disp_cmd != DISP_NONE && s_client) {
        esp_mqtt_client_publish(s_client, NETPIE_TOPIC_SET,
                                "{\"data\":{\"disp_req\":\"0,0\"}}", 0, 0, 0);
    }
}

/* Push every scalar of s_shadow to NETPIE so the cloud aligns to the
 * device's local state. Called on MQTT connect — the device's touch
 * screen is the source of truth (user spec 2026-05-15), so any drift on
 * the cloud side gets overwritten before we'd have to handle it through
 * the pending-approval flow.
 *
 * Implementation: take a copy of s_shadow under lock, then publish each
 * scalar without holding the lock (publish path is non-blocking but
 * still touches MQTT internals; safer not to nest). NETPIE will echo
 * everything back on @shadow/data/updated — parse_shadow's diff filter
 * sees matching values and silently no-ops, but first_sync flips on the
 * first echo so subsequent web-widget edits go through pending. */
static void publish_full_shadow_from_copy(const netpie_shadow_t *src)
{
    if (!src || !src->loaded) return;
    publish_shadow_payload(build_shadow_payload_int("scheduleEnabled", src->enabled ? 1 : 0));
    publish_shadow_payload(build_shadow_payload_int("max_pills", src->max_pills));
    for (int i = 0; i < 7; i++) {
        publish_shadow_payload(build_shadow_payload_string(s_slot_keys[i], src->slot_time[i]));
    }
    for (int i = 0; i < DISPENSER_MED_COUNT; i++) {
        char key[24];
        int id = i + 1;
        snprintf(key, sizeof(key), "med%d_name", id);
        publish_shadow_payload(build_shadow_payload_string(key, src->med[i].name));
        snprintf(key, sizeof(key), "med%d_count", id);
        publish_shadow_payload(build_shadow_payload_int(key, src->med[i].count));
        snprintf(key, sizeof(key), "med%d_slots", id);
        publish_shadow_payload(build_shadow_payload_int(key, src->med[i].slots));
    }
}

static void netpie_push_local_to_cloud(void)
{
    netpie_shadow_t snap;
    if (!netpie_shadow_copy(&snap)) return;
    publish_full_shadow_from_copy(&snap);
    ESP_LOGI(TAG, "Pushed local shadow to NETPIE (device-is-master sync)");
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
            /* Device-is-master sync: push local shadow up before doing
             * anything else, so the cloud immediately reflects the
             * device's current state. Skips the legacy GET-then-apply
             * round-trip which could overwrite local NVS with stale
             * cloud values if the operator made offline edits via touch. */
            netpie_push_local_to_cloud();
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

                bool is_get_resp = (strstr(topic, "@shadow/data/get/response") != NULL);
                bool is_updated  = (strstr(topic, "@shadow/data/updated")      != NULL);
                if (is_get_resp || is_updated) {
                    char *buf = (char *)malloc(ev->data_len + 1);
                    if (buf) {
                        memcpy(buf, ev->data, ev->data_len);
                        buf[ev->data_len] = '\0';
                        /* Only GET responses are authoritative initial pulls;
                         * /updated is a third-party write that must go through
                         * the pending-approval diff path. */
                        parse_shadow(buf, is_get_resp);
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

bool netpie_shadow_update_slot(int slot_idx, const char *hh_mm)
{
    if (!hh_mm || slot_idx < 0 || slot_idx >= 7) return false;

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
            return false;  // malformed
        }
        int hh = (hh_mm[0]-'0')*10 + (hh_mm[1]-'0');
        int mm = (hh_mm[3]-'0')*10 + (hh_mm[4]-'0');
        if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
        strlcpy(safe_time, hh_mm, sizeof(safe_time));
    }

    bool ok = false;
    if (shadow_lock()) {
        /* Validate the FULL set with the candidate slot patched in.
         * Disabled-slot writes ("--:--") always pass; otherwise reject
         * if the resulting set would break pre<post within a meal or
         * duplicate another set slot's HH:MM. The caller surfaces the
         * failure on the touch UI / web — silent reject would leave
         * the operator confused why the new time didn't stick. */
        const char *cur[7];
        for (int i = 0; i < 7; i++) cur[i] = s_shadow.slot_time[i];
        if (slot_times_monotonic(cur, slot_idx, safe_time)) {
            strlcpy(s_shadow.slot_time[slot_idx], safe_time,
                    sizeof(s_shadow.slot_time[slot_idx]));
            shadow_cache_save_locked();
            ok = true;
        } else {
            ESP_LOGW(TAG, "Rejected slot %d='%s' — duplicate or pre>=post",
                     slot_idx, safe_time);
        }
        shadow_unlock();
    }

    if (ok) {
        publish_shadow_payload(build_shadow_payload_string(s_slot_keys[slot_idx], safe_time));
    }
    return ok;
}

/* Atomically write TWO slot times under a single shadow_lock,
 * validating the merged state once. The single-slot path
 * (netpie_shadow_update_slot) trips over its own cascade because the
 * pair write is validated against the still-old self value — moving
 * a "before" slot down rejects the pair update, moving it up rejects
 * the self update. This helper sidesteps that by patching BOTH
 * candidates into a temp array before calling slot_times_monotonic,
 * so the pair-gap / pre<post / 60-min-cap rules see the final state.
 * On reject NOTHING is written (no partial half-applied state).
 * Same accept criteria as the single-slot path; "--:--" disables. */
bool netpie_shadow_update_slot_pair(int idx_a, const char *val_a,
                                    int idx_b, const char *val_b)
{
    if (!val_a || !val_b) return false;
    if (idx_a < 0 || idx_a >= 7 || idx_b < 0 || idx_b >= 7) return false;
    if (idx_a == idx_b) return false;

    /* Normalize both inputs through the same format guard the single
     * path uses. Disabled-slot ("--:--") is the only non-HH:MM form
     * accepted. */
    char safe_a[6] = {0};
    char safe_b[6] = {0};
    const char *inputs[2] = { val_a, val_b };
    char *outs[2]         = { safe_a, safe_b };
    for (int k = 0; k < 2; k++) {
        const char *s = inputs[k];
        char *out     = outs[k];
        if (strcmp(s, "--:--") == 0 || s[0] == '\0') {
            strlcpy(out, "--:--", 6);
            continue;
        }
        if (strlen(s) != 5 || s[2] != ':' ||
            !isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1]) ||
            !isdigit((unsigned char)s[3]) || !isdigit((unsigned char)s[4])) {
            return false;
        }
        int hh = (s[0]-'0')*10 + (s[1]-'0');
        int mm = (s[3]-'0')*10 + (s[4]-'0');
        if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
        strlcpy(out, s, 6);
    }

    bool ok = false;
    if (shadow_lock()) {
        const char *cand[7];
        for (int i = 0; i < 7; i++) cand[i] = s_shadow.slot_time[i];
        cand[idx_a] = safe_a;
        cand[idx_b] = safe_b;
        /* Validate the merged candidate array (slot_idx=-1 path). */
        if (slot_times_monotonic(cand, -1, NULL)) {
            strlcpy(s_shadow.slot_time[idx_a], safe_a,
                    sizeof(s_shadow.slot_time[idx_a]));
            strlcpy(s_shadow.slot_time[idx_b], safe_b,
                    sizeof(s_shadow.slot_time[idx_b]));
            shadow_cache_save_locked();
            ok = true;
        } else {
            ESP_LOGW(TAG, "Rejected pair %d='%s', %d='%s' — invalid combined state",
                     idx_a, safe_a, idx_b, safe_b);
        }
        shadow_unlock();
    }

    if (ok) {
        publish_shadow_payload(build_shadow_payload_string(s_slot_keys[idx_a], safe_a));
        publish_shadow_payload(build_shadow_payload_string(s_slot_keys[idx_b], safe_b));
    }
    return ok;
}

bool netpie_slot_time_valid(int slot_idx, const char *hh_mm)
{
    if (slot_idx < 0 || slot_idx >= 7 || !hh_mm) return false;
    /* Disabled / empty always allowed. */
    if (hh_mm[0] == '\0' || strcmp(hh_mm, "--:--") == 0) return true;

    if (!shadow_lock()) return false;
    const char *cur[7];
    for (int i = 0; i < 7; i++) cur[i] = s_shadow.slot_time[i];
    bool ok = slot_times_monotonic(cur, slot_idx, hh_mm);
    shadow_unlock();
    return ok;
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

void netpie_shadow_update_max_pills(int new_max)
{
    if (new_max < 1)   new_max = 1;
    if (new_max > 999) new_max = 999;
    if (shadow_lock()) {
        s_shadow.max_pills = new_max;
        shadow_cache_save_locked();
        shadow_unlock();
    }
    publish_shadow_payload(build_shadow_payload_int("max_pills", new_max));
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

bool netpie_pending_active(void)
{
    bool active = false;
    if (!shadow_lock()) return false;
    active = s_pending.active;
    shadow_unlock();
    return active;
}

bool netpie_pending_get(netpie_pending_t *out)
{
    if (!out) return false;
    if (!shadow_lock()) return false;
    if (!s_pending.active) {
        shadow_unlock();
        return false;
    }
    *out = s_pending;
    shadow_unlock();
    return true;
}

void netpie_pending_approve(void)
{
    if (!shadow_lock()) return;
    if (!s_pending.active) {
        shadow_unlock();
        return;
    }

    /* Apply only the fields the operator was actually asked about. */
    if (s_pending.enabled_diff) {
        s_shadow.enabled = s_pending.snapshot.enabled;
    }
    if (s_pending.max_pills_diff) {
        s_shadow.max_pills = s_pending.snapshot.max_pills;
    }
    for (int i = 0; i < 7; i++) {
        if (s_pending.slot_time_diff[i]) {
            strlcpy(s_shadow.slot_time[i], s_pending.snapshot.slot_time[i],
                    sizeof(s_shadow.slot_time[i]));
        }
    }
    for (int i = 0; i < DISPENSER_MED_COUNT; i++) {
        if (s_pending.med_name_diff[i]) {
            strlcpy(s_shadow.med[i].name, s_pending.snapshot.med[i].name,
                    sizeof(s_shadow.med[i].name));
        }
        if (s_pending.med_count_diff[i]) {
            s_shadow.med[i].count = s_pending.snapshot.med[i].count;
        }
        if (s_pending.med_slots_diff[i]) {
            s_shadow.med[i].slots = s_pending.snapshot.med[i].slots;
        }
    }

    memset(&s_pending, 0, sizeof(s_pending));
    s_pending_notify_telegram = false;
    shadow_cache_save_locked();
    ESP_LOGI(TAG, "NETPIE pending shadow approved + applied");
    /* Cloud already has these values (it sent them to us), so no
     * republish needed. shadow_unlock() publishes the local snapshot. */
    shadow_unlock();
}

void netpie_pending_reject(void)
{
    /* Snapshot the current shadow inside the lock so the republish
     * below uses a consistent state even if a concurrent touch update
     * mutates s_shadow afterwards. */
    netpie_shadow_t snap = {0};
    bool had_pending = false;

    if (!shadow_lock()) return;
    if (s_pending.active) {
        had_pending = true;
        snap = s_shadow;
        memset(&s_pending, 0, sizeof(s_pending));
        s_pending_notify_telegram = false;
    }
    shadow_unlock();

    if (!had_pending) return;

    ESP_LOGI(TAG, "NETPIE pending shadow rejected — reverting cloud state");

    /* Republish every scalar so the cloud snaps back to local state.
     * Each republish triggers a NETPIE echo that re-enters parse_shadow,
     * where the diff check finds zero changes vs s_shadow and silently
     * drops it — no new pending event. */
    publish_shadow_payload(build_shadow_payload_int("scheduleEnabled",
                                                   snap.enabled ? 1 : 0));
    publish_shadow_payload(build_shadow_payload_int("max_pills", snap.max_pills));
    for (int i = 0; i < 7; i++) {
        publish_shadow_payload(build_shadow_payload_string(s_slot_keys[i],
                                                           snap.slot_time[i]));
    }
    for (int i = 0; i < DISPENSER_MED_COUNT; i++) {
        char key[24];
        int id = i + 1;
        snprintf(key, sizeof(key), "med%d_name", id);
        publish_shadow_payload(build_shadow_payload_string(key, snap.med[i].name));
        snprintf(key, sizeof(key), "med%d_count", id);
        publish_shadow_payload(build_shadow_payload_int(key, snap.med[i].count));
        snprintf(key, sizeof(key), "med%d_slots", id);
        publish_shadow_payload(build_shadow_payload_int(key, snap.med[i].slots));
    }
}

bool netpie_pending_take_telegram_notify(void)
{
    /* Single-consumer read-and-clear. The flag goes true only on the
     * inactive→active transition in parse_shadow, so we don't need a
     * compare-and-swap — at worst we miss one stale read. */
    if (!s_pending_notify_telegram) return false;
    s_pending_notify_telegram = false;
    return true;
}

/* Format the diff in Thai. Returns length written (excluding NUL) or 0
 * if no pending. Truncates silently if out is too small. */
size_t netpie_pending_format_summary_th(char *out, size_t out_cap)
{
    if (!out || out_cap == 0) return 0;
    out[0] = '\0';

    netpie_pending_t p;
    if (!netpie_pending_get(&p)) return 0;

    netpie_shadow_t cur;
    if (!netpie_shadow_copy(&cur)) return 0;

    size_t len = 0;
    #define APPEND(...) do { \
        if (len < out_cap - 1) { \
            int w = snprintf(out + len, out_cap - len, __VA_ARGS__); \
            if (w > 0) len += (size_t)w; \
            if (len >= out_cap) len = out_cap - 1; \
        } \
    } while (0)

    if (p.enabled_diff) {
        APPEND("• ตารางจ่ายยา: %s → %s\n",
               cur.enabled ? "เปิด" : "ปิด",
               p.snapshot.enabled ? "เปิด" : "ปิด");
    }
    if (p.max_pills_diff) {
        APPEND("• ยาสูงสุด/โมดูล: %d → %d\n",
               cur.max_pills, p.snapshot.max_pills);
    }
    for (int i = 0; i < 7; i++) {
        if (p.slot_time_diff[i]) {
            APPEND("• มื้อ %d: %s → %s\n",
                   i + 1, cur.slot_time[i], p.snapshot.slot_time[i]);
        }
    }
    for (int i = 0; i < DISPENSER_MED_COUNT; i++) {
        if (p.med_name_diff[i]) {
            APPEND("• โมดูล %d ชื่อ: \"%s\" → \"%s\"\n",
                   i + 1, cur.med[i].name, p.snapshot.med[i].name);
        }
        if (p.med_count_diff[i]) {
            APPEND("• โมดูล %d จำนวน: %d → %d\n",
                   i + 1, cur.med[i].count, p.snapshot.med[i].count);
        }
        if (p.med_slots_diff[i]) {
            APPEND("• โมดูล %d มื้อ(bitmask): 0x%02X → 0x%02X\n",
                   i + 1, cur.med[i].slots, p.snapshot.med[i].slots);
        }
    }
    #undef APPEND
    return len;
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
