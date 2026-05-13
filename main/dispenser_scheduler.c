// ─────────────────────────────────────────────────────────────────────────────
//  dispenser_scheduler.c — Medicine dispenser timing logic
//
//  ทำงานอย่างไร:
//  1. Task วน loop ทุก 30 วินาที
//  2. อ่านเวลาจาก DS3231 RTC
//  3. เทียบกับ 7 time slots จาก NETPIE shadow
//  4. ถ้าตรง slot (match ภายใน ±1 นาที) และ scheduleEnabled=1:
//     - วนดู med 1-6 ว่า slot bit ตั้งไว้ไหม?
//     - ถ้า count > 0 → servo go_work → delay 1s → go_home → count--
//     - อัปเดต shadow count
//  5. แต่ละ slot จะ trigger ได้ครั้งเดียวต่อนาที (กัน re-trigger)
// ─────────────────────────────────────────────────────────────────────────────

#include "dispenser_scheduler.h"
#include "nvs.h"
#include "netpie_mqtt.h"
#include "ds3231.h"
#include "pca9685.h"
#include "pcf8574.h"
#include "config.h"
#include "telegram_bot.h"
#include "jpeg_encoder.h"
#include "camera_init.h"
#include "vl53l0x_multi.h"
#include "dfplayer.h"
#include "cloud_secrets.h"
#include "offline_sync.h"
#include "wifi_sta.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Removed g_pre_alert_minutes from here
// Global volume state is now managed dynamically within dfplayer.c directly

static const char *TAG = "dispenser";
static const uint32_t STOCK_AUDIT_IDLE_MS = 2500;
static const uint32_t DISPENSER_TASK_STACK_SIZE = 8192;
static const uint32_t MANUAL_DISPENSE_TASK_STACK_SIZE = 6144;

// label แต่ละ slot (ตรงกับ HTML)
static const char *SLOT_LABELS[7] = {
    "Before Breakfast", "After Breakfast", "Before Lunch", "After Lunch", "Before Dinner", "After Dinner", "Bedtime"
};

// ป้องกัน re-trigger ใน 1 นาทีเดียวกัน. Bumped 6 → 7 so the
// HH:MM string always has room for the trailing null even if a future
// "HH:MM\0" snprintf writes the full 5 chars + null cleanly.
static char s_last_triggered[7] = "";   // "HH:MM" ที่ trigger ล่าสุด

// Per-slot last-trigger epoch. Lets the scheduler tolerate small
// wake delays (e.g., when dispenser_task is busy with a previous
// dose) without missing the next slot — particularly at midnight,
// where the previous global s_last_triggered approach combined with
// the strict diff==0 check could miss a 00:00 slot if the task woke
// at 00:01. We accept any minute >= slot_time within a 5-min grace
// window AND only fire once per slot per 12-hour window.
static time_t s_slot_last_fire[7] = {0};
#define SLOT_GRACE_MIN          5     // accept up to 5 min late
#define SLOT_REFIRE_GUARD_SEC   (12 * 3600)

// cache next dose string สำหรับ display
static char s_next_dose[64] = "No schedule";

// track missed slots for today
static uint8_t g_missed_slots_mask = 0;

uint8_t dispenser_get_missed_slots(void) {
    return g_missed_slots_mask;
}

typedef struct {
    bool pending;
    int start_count;
    int last_count;
    TickType_t last_change_tick;
} stock_audit_state_t;

static stock_audit_state_t s_stock_audit[DISPENSER_MED_COUNT];
static portMUX_TYPE s_stock_audit_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_dispense_mutex = NULL;
static portMUX_TYPE s_dispense_state_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_dispense_busy = false;
static bool s_low_stock_alert_sent[DISPENSER_MED_COUNT] = {0};

// Emergency stop flag — when set, no new dispense (manual or scheduled)
// will start. In-flight servo motion runs to completion. Persisted via
// NVS so an auto-restart while stopped stays stopped.
static volatile bool s_emergency_stop = false;

// Per-module IR sensor presence. Default true — IR is the authoritative
// pill counter for this dispenser (servo cycles alone are not trusted,
// because the user wants the system to refuse to "count" anything that
// the IR beam didn't physically see). When IR is genuinely not wired
// for a slot, disable it per-slot via the UI / /tech endpoint so that
// slot's strict-IR loop bails immediately instead of running blind.
static bool s_ir_present[DISPENSER_MED_COUNT] = {
    true, true, true, true, true, true,
};

bool dispenser_ir_present(int med_idx)
{
    if (med_idx < 0 || med_idx >= DISPENSER_MED_COUNT) return true;
    return s_ir_present[med_idx];
}

static void dispenser_ir_save_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("dispenser", NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t mask = 0;
    for (int i = 0; i < DISPENSER_MED_COUNT; ++i) {
        if (s_ir_present[i]) mask |= (1 << i);
    }
    nvs_set_u8(h, "ir_mask", mask);
    nvs_commit(h);
    nvs_close(h);
}

static void dispenser_ir_load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("dispenser", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t mask = 0xFF;  // default: all IR present (matches s_ir_present[] init)
    nvs_get_u8(h, "ir_mask", &mask);
    nvs_close(h);
    for (int i = 0; i < DISPENSER_MED_COUNT; ++i) {
        s_ir_present[i] = (mask & (1 << i)) != 0;
    }
}

void dispenser_ir_set_present(int med_idx, bool present)
{
    if (med_idx < 0 || med_idx >= DISPENSER_MED_COUNT) return;
    if (s_ir_present[med_idx] == present) return;
    s_ir_present[med_idx] = present;
    dispenser_ir_save_nvs();
    ESP_LOGI(TAG, "IR sensor for med%d %s",
             med_idx + 1, present ? "ENABLED" : "DISABLED (servo-trust mode)");
}

/* ── IR calibration: run one cycle and record IR samples ── */
esp_err_t dispenser_ir_calibrate(int med_idx,
                                 ir_cal_sample_t *samples,
                                 int max_samples,
                                 int *out_count)
{
    if (!samples || max_samples <= 0 || !out_count) return ESP_ERR_INVALID_ARG;
    if (med_idx < 0 || med_idx >= DISPENSER_MED_COUNT) return ESP_ERR_INVALID_ARG;
    *out_count = 0;
    if (!s_dispense_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_dispense_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "IR calibrate: med%d", med_idx + 1);
    int count = 0;
    uint8_t bit_mask = (uint8_t)(1 << med_idx);
    uint32_t cycle_start = esp_log_timestamp();

    pca9685_go_work(med_idx);
    while ((esp_log_timestamp() - cycle_start) < 1500) {
        uint8_t ir = 0xFF;
        if (pcf8574_read(&ir) == ESP_OK && count < max_samples) {
            samples[count].time_ms  = esp_log_timestamp() - cycle_start;
            samples[count].raw_byte = ir;
            samples[count].bit_low  = ((ir & bit_mask) == 0) ? 1 : 0;
            count++;
        }
        vTaskDelay(1);
    }
    pca9685_go_home(med_idx);
    while ((esp_log_timestamp() - cycle_start) < 5500) {
        uint8_t ir = 0xFF;
        if (pcf8574_read(&ir) == ESP_OK && count < max_samples) {
            samples[count].time_ms  = esp_log_timestamp() - cycle_start;
            samples[count].raw_byte = ir;
            samples[count].bit_low  = ((ir & bit_mask) == 0) ? 1 : 0;
            count++;
        }
        vTaskDelay(1);
    }

    xSemaphoreGive(s_dispense_mutex);
    *out_count = count;
    ESP_LOGI(TAG, "IR calibrate done: med%d %d samples captured", med_idx + 1, count);
    return ESP_OK;
}

// Audit ring: holds dispense/return/edit events for /audit.json AND for
// the Telegram /log command. Expanded from 32 to 256 (≈ 2-3 months of
// typical 3-doses/day use) and persisted to NVS so it survives reboots
// — Google Sheets dependency is dropped, the device is now the source
// of truth for its own dose history. NVS blob ~3 KB worst case.
//
// Save strategy: every audit_log() writes the full ring back to NVS.
// NVS auto-wear-levels and 4-10 doses/day = ~1500-4000 writes/year is
// well within flash endurance (10^4 cycles per page, ~10 pages used).
#define AUDIT_RING_SIZE 256
static dispenser_audit_entry_t s_audit_ring[AUDIT_RING_SIZE];
static int s_audit_head = 0;     // next write index
static int s_audit_count = 0;    // populated entries (capped at SIZE)
static portMUX_TYPE s_audit_mux = portMUX_INITIALIZER_UNLOCKED;

#define AUDIT_NVS_NAMESPACE "dispenser"
#define AUDIT_NVS_KEY_RING  "audit_ring"
#define AUDIT_NVS_KEY_HEAD  "audit_head"
#define AUDIT_NVS_KEY_COUNT "audit_count"

static void dispenser_audit_save_nvs(void)
{
    /* Snapshot under the critical section, then write to NVS outside
     * it (NVS can block ~10-100 ms on flash erase). The snapshot buffer
     * is STATIC (3 KB) instead of stack-local — this function is
     * reachable from manual_dispense_task whose 6 KB stack was getting
     * dangerously close to overflow with a 3 KB stack array plus the
     * ~2 KB msg buffers + ESP_LOGI call chains down the dispense
     * pipeline. Static + mutex makes it safe across callers. */
    static dispenser_audit_entry_t s_audit_snapshot[AUDIT_RING_SIZE];
    static SemaphoreHandle_t s_audit_snap_mtx = NULL;
    if (!s_audit_snap_mtx) {
        /* One-shot lazy create. Not racy because the first audit_log()
         * call comes from a task context after scheduler init. */
        s_audit_snap_mtx = xSemaphoreCreateMutex();
        if (!s_audit_snap_mtx) return;
    }
    if (xSemaphoreTake(s_audit_snap_mtx, pdMS_TO_TICKS(200)) != pdTRUE) {
        /* Another caller is mid-save; drop this one — the next event
         * will catch us up. Avoids stacking blocked tasks behind a
         * slow flash erase. */
        return;
    }

    int head_snap, count_snap;
    taskENTER_CRITICAL(&s_audit_mux);
    memcpy(s_audit_snapshot, s_audit_ring, sizeof(s_audit_snapshot));
    head_snap  = s_audit_head;
    count_snap = s_audit_count;
    taskEXIT_CRITICAL(&s_audit_mux);

    nvs_handle_t h;
    if (nvs_open(AUDIT_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, AUDIT_NVS_KEY_RING, s_audit_snapshot, sizeof(s_audit_snapshot));
        nvs_set_i32(h, AUDIT_NVS_KEY_HEAD,  head_snap);
        nvs_set_i32(h, AUDIT_NVS_KEY_COUNT, count_snap);
        nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(s_audit_snap_mtx);
}

void dispenser_audit_load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(AUDIT_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t blob_sz = sizeof(s_audit_ring);
    int32_t head_loaded  = 0;
    int32_t count_loaded = 0;
    esp_err_t r1 = nvs_get_blob(h, AUDIT_NVS_KEY_RING, s_audit_ring, &blob_sz);
    esp_err_t r2 = nvs_get_i32 (h, AUDIT_NVS_KEY_HEAD,  &head_loaded);
    esp_err_t r3 = nvs_get_i32 (h, AUDIT_NVS_KEY_COUNT, &count_loaded);
    nvs_close(h);

    if (r1 == ESP_OK && r2 == ESP_OK && r3 == ESP_OK &&
        blob_sz == sizeof(s_audit_ring) &&
        head_loaded  >= 0 && head_loaded  < AUDIT_RING_SIZE &&
        count_loaded >= 0 && count_loaded <= AUDIT_RING_SIZE) {
        s_audit_head  = (int)head_loaded;
        s_audit_count = (int)count_loaded;
        ESP_LOGI(TAG, "Audit ring restored from NVS: %d entries", s_audit_count);

        /* Age-based auto-purge: drop entries older than 60 days. The
         * ring already overwrites oldest-first when full (256 entries
         * ≈ 30-40 days at typical use), but on a lightly-used device
         * stale rows from many months back can hang around. Purging on
         * boot keeps /log tidy without any user action. Only runs when
         * the wall clock is plausibly synced (year >= 2024) — if RTC
         * isn't set yet we'd wrongly purge everything. */
        time_t now = time(NULL);
        struct tm now_tm = {0};
        bool clock_synced = false;
#if defined(_WIN32)
        if (localtime_s(&now_tm, &now) == 0 && now_tm.tm_year >= 124) clock_synced = true;
#else
        if (localtime_r(&now, &now_tm) && now_tm.tm_year >= 124) clock_synced = true;
#endif
        if (clock_synced) {
            const uint32_t AGE_LIMIT_SEC = 60u * 24u * 3600u;  /* 60 days */
            int kept = 0;
            int total = s_audit_count;
            for (int k = 0; k < total; ++k) {
                int src_idx = (s_audit_head - total + k + AUDIT_RING_SIZE) % AUDIT_RING_SIZE;
                uint32_t ts = s_audit_ring[src_idx].timestamp;
                if (ts == 0 || (now - (time_t)ts) > AGE_LIMIT_SEC) continue;
                if (kept != k) {
                    int dst_idx = (s_audit_head - total + kept + AUDIT_RING_SIZE) % AUDIT_RING_SIZE;
                    s_audit_ring[dst_idx] = s_audit_ring[src_idx];
                }
                ++kept;
            }
            if (kept != total) {
                /* Re-pack so the kept entries sit at the end of the ring. */
                dispenser_audit_entry_t tmp[AUDIT_RING_SIZE];
                for (int k = 0; k < kept; ++k) {
                    int src_idx = (s_audit_head - total + k + AUDIT_RING_SIZE) % AUDIT_RING_SIZE;
                    tmp[k] = s_audit_ring[src_idx];
                }
                memset(s_audit_ring, 0, sizeof(s_audit_ring));
                memcpy(s_audit_ring, tmp, kept * sizeof(tmp[0]));
                s_audit_head  = kept % AUDIT_RING_SIZE;
                s_audit_count = kept;
                ESP_LOGI(TAG, "Audit purged %d stale rows (>60 days), kept %d",
                         total - kept, kept);
            }
        }
    } else if (r1 != ESP_OK) {
        ESP_LOGI(TAG, "Audit ring: no prior NVS data, starting fresh");
    } else {
        ESP_LOGW(TAG, "Audit ring NVS read partial/corrupt — discarding "
                      "(r1=%d r2=%d r3=%d blob=%u)",
                 r1, r2, r3, (unsigned)blob_sz);
        memset(s_audit_ring, 0, sizeof(s_audit_ring));
        s_audit_head  = 0;
        s_audit_count = 0;
    }
}

void dispenser_audit_log(int med_idx, int from_count, int to_count, char source)
{
    if (med_idx < 0 || med_idx >= DISPENSER_MED_COUNT) return;
    /* 'L' (missed dose) and 'N' (no-stock skip) record events where the
     * count did NOT change — they're the negative-result counterparts of
     * 'M'/'S' (manual / scheduled dispense success). For every other
     * source, suppress no-op rows so the ring doesn't fill with noise. */
    if (from_count == to_count && source != 'L' && source != 'N') return;
    time_t now = 0;
    time(&now);
    taskENTER_CRITICAL(&s_audit_mux);
    s_audit_ring[s_audit_head] = (dispenser_audit_entry_t){
        .timestamp  = (uint32_t)now,
        .med_idx    = (int16_t)med_idx,
        .from_count = (int16_t)from_count,
        .to_count   = (int16_t)to_count,
        .source     = source ? source : 'X',
    };
    s_audit_head = (s_audit_head + 1) % AUDIT_RING_SIZE;
    if (s_audit_count < AUDIT_RING_SIZE) s_audit_count++;
    taskEXIT_CRITICAL(&s_audit_mux);

    /* Persist immediately so a reboot loses ZERO history. NVS write is
     * a few ms of flash op; runs in the caller's context which is
     * always a low-priority task (dispenser/manual/web/UI) — never an
     * ISR. */
    dispenser_audit_save_nvs();
}

size_t dispenser_audit_get(dispenser_audit_entry_t *out_entries, size_t max_entries)
{
    if (!out_entries || max_entries == 0) return 0;
    size_t written = 0;
    taskENTER_CRITICAL(&s_audit_mux);
    int idx = s_audit_head - 1;
    if (idx < 0) idx += AUDIT_RING_SIZE;
    int remaining = s_audit_count;
    while (remaining > 0 && written < max_entries) {
        out_entries[written++] = s_audit_ring[idx];
        idx--;
        if (idx < 0) idx += AUDIT_RING_SIZE;
        remaining--;
    }
    taskEXIT_CRITICAL(&s_audit_mux);
    return written;
}

size_t dispenser_audit_count(void)
{
    return (size_t)s_audit_count;
}

static void dispenser_emergency_save_nvs(bool active)
{
    nvs_handle_t h;
    if (nvs_open("dispenser", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "estop", active ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void dispenser_emergency_load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("dispenser", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "estop", &v) == ESP_OK) {
            s_emergency_stop = (v != 0);
        }
        nvs_close(h);
    }
    if (s_emergency_stop) {
        // Make this loud at boot so a stuck-stopped device after a
        // panic isn't mistaken for "scheduler running normally but no
        // doses fire" — that combination produced support tickets.
        ESP_LOGW(TAG, "Boot with EMERGENCY STOP active from previous session — "
                      "dispenser will not fire until /resume or web Clear");
    }

    /* Restore per-slot refire guards. Without this, rebooting just
     * after a slot fired today would re-fire the same dose because
     * s_slot_last_fire[] was RAM-only. The 12-hour guard then runs
     * against the persisted timestamp so the Confirm popup doesn't
     * pop up again on boot. Tolerates missing/short blob gracefully. */
    {
        nvs_handle_t h;
        if (nvs_open("dispenser", NVS_READONLY, &h) == ESP_OK) {
            size_t sz = sizeof(s_slot_last_fire);
            if (nvs_get_blob(h, "slot_lastfire", s_slot_last_fire, &sz) != ESP_OK ||
                sz != sizeof(s_slot_last_fire)) {
                memset(s_slot_last_fire, 0, sizeof(s_slot_last_fire));
            }
            nvs_close(h);
        }
    }
}

void dispenser_emergency_set(void)
{
    if (s_emergency_stop) return;
    s_emergency_stop = true;
    dispenser_emergency_save_nvs(true);
    ESP_LOGW(TAG, "EMERGENCY STOP set — no new dispense will start");
}

void dispenser_emergency_clear(void)
{
    if (!s_emergency_stop) return;
    s_emergency_stop = false;
    dispenser_emergency_save_nvs(false);
    ESP_LOGI(TAG, "Emergency stop cleared — dispenser back online");
}

bool dispenser_emergency_active(void)
{
    return s_emergency_stop;
}

// Quiet hours: minutes-of-day window during which scheduled dispense is
// suppressed. -1/-1 or start==end means disabled. Window may wrap
// midnight (start > end). Manual + Telegram /dispense bypass the gate.
static int s_quiet_start_min = 0;
static int s_quiet_end_min = 0;

static void quiet_hours_save_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("dispenser", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i16(h, "qh_start", (int16_t)s_quiet_start_min);
        nvs_set_i16(h, "qh_end",   (int16_t)s_quiet_end_min);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void quiet_hours_load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("dispenser", NVS_READONLY, &h) == ESP_OK) {
        int16_t s = 0, e = 0;
        nvs_get_i16(h, "qh_start", &s);
        nvs_get_i16(h, "qh_end", &e);
        if (s >= 0 && s < 1440) s_quiet_start_min = s;
        if (e >= 0 && e < 1440) s_quiet_end_min   = e;
        nvs_close(h);
    }
}

void dispenser_set_quiet_hours(int start_min, int end_min)
{
    if (start_min < 0 || start_min >= 1440) start_min = 0;
    if (end_min < 0 || end_min >= 1440) end_min = 0;
    s_quiet_start_min = start_min;
    s_quiet_end_min = end_min;
    quiet_hours_save_nvs();
    ESP_LOGI(TAG, "Quiet hours set: %02d:%02d → %02d:%02d (%s)",
             start_min / 60, start_min % 60,
             end_min / 60, end_min % 60,
             (start_min == end_min) ? "disabled" : "active");
}

void dispenser_get_quiet_hours(int *start_min, int *end_min)
{
    if (start_min) *start_min = s_quiet_start_min;
    if (end_min) *end_min = s_quiet_end_min;
}

bool dispenser_in_quiet_hours(int cur_h, int cur_m)
{
    if (s_quiet_start_min == s_quiet_end_min) return false;  // disabled
    int now = cur_h * 60 + cur_m;
    if (s_quiet_start_min < s_quiet_end_min) {
        // Same-day window e.g. 13:00 → 14:00.
        return (now >= s_quiet_start_min && now < s_quiet_end_min);
    }
    // Wrap-midnight window e.g. 22:00 → 06:00 (now >= 22:00 OR now < 6:00).
    return (now >= s_quiet_start_min || now < s_quiet_end_min);
}

static const char *TG_SLOT_LABELS_TH[7] = {
    "ก่อนอาหารเช้า", "หลังอาหารเช้า", "ก่อนอาหารกลางวัน", "หลังอาหารกลางวัน",
    "ก่อนอาหารเย็น", "หลังอาหารเย็น", "ก่อนนอน"
};

/* ── Google Sheets Log Task ── */
void google_sheets_log(const char *event, const char *meds, const char *detail) {
    if (!cloud_secrets_has_google_script()) return;

    offline_sync_queue_google_sheets(event ? event : "-",
                                     meds ? meds : "-",
                                     detail ? detail : "-");

    if (wifi_sta_connected()) {
        offline_sync_flush_async();
    }
}

/* ── แปลง "HH:MM" เป็น HH และ MM ── */
static bool parse_hhmm(const char *s, int *h, int *m)
{
    if (!s || strlen(s) < 5 || s[2] != ':') return false;
    *h = atoi(s);
    *m = atoi(s + 3);
    return (*h >= 0 && *h < 24 && *m >= 0 && *m < 60);
}

/* ── คำนวณกี่นาทีจนถึง slot ถัดไป ── */
static int minutes_until(int cur_h, int cur_m, int tgt_h, int tgt_m)
{
    int cur = cur_h * 60 + cur_m;
    int tgt = tgt_h * 60 + tgt_m;
    if (tgt < cur) tgt += 24 * 60;  // ข้ามวัน
    return tgt - cur;
}

/* Forward decl — definition is below in this file. */
static bool telegram_lang_is_th(void);

/* ── อัปเดต next dose string ── */
static void update_next_dose_str(int cur_h, int cur_m)
{
    const netpie_shadow_t *sh = netpie_get_shadow();
    if (!sh || !sh->enabled || !sh->loaded) {
        snprintf(s_next_dose, sizeof(s_next_dose), "No schedule");
        return;
    }

    int best_min = 99999;
    int best_slot = -1;

    for (int s = 0; s < 7; s++) {
        int th, tm;
        if (!parse_hhmm(sh->slot_time[s], &th, &tm)) continue;
        // ตรวจว่า slot นี้มียาอย่างน้อย 1 ตลับ
        bool has_med = false;
        for (int i = 0; i < DISPENSER_MED_COUNT; i++) {
            if ((sh->med[i].slots >> s) & 1 && sh->med[i].count > 0) {
                has_med = true; break;
            }
        }
        if (!has_med) continue;
        int diff = minutes_until(cur_h, cur_m, th, tm);
        if (diff > 0 && diff < best_min) { best_min = diff; best_slot = s; }
    }

    if (best_slot < 0) {
        snprintf(s_next_dose, sizeof(s_next_dose), "No schedule");
    } else {
        int minutes_to_midnight = (24 * 60) - (cur_h * 60 + cur_m);
        bool is_tomorrow = (best_min > minutes_to_midnight);

        if (is_tomorrow) {
            // All of today's doses are done — show only a short language-
            // matched label. Mixing the English SLOT_LABELS into a Thai-
            // rendered line caused garbled output in EN mode and a clunky
            // "พรุ่งนี้ Bedtime" mix in TH mode.
            snprintf(s_next_dose, sizeof(s_next_dose), "%s",
                     telegram_lang_is_th() ? "รอพรุ่งนี้" : "Tomorrow");
        } else {
            snprintf(s_next_dose, sizeof(s_next_dose), "%s  %s",
                     SLOT_LABELS[best_slot],
                     netpie_get_shadow()->slot_time[best_slot]);
        }
    }
}

static bool s_waiting_confirm = false;
static bool s_empty_stock_warning = false;
static uint32_t s_wait_start_ticks = 0;
static int s_pending_slot_idx = -1;
static bool s_dispense_approved = false;

// Tick recorded when s_dispense_busy was set true. Used by the watchdog
// in dispenser_task to detect a stuck busy state (servo hang, I2C
// wedge, task crash mid-operation) and recover instead of bricking
// the whole dispenser. Bumped from 30s → 90s after observing that a
// 6-med slot takes ~25s for servo + IR wait + audio play, leaving no
// safety margin and risking a watchdog fire mid-dispense.
static TickType_t s_dispense_busy_since = 0;
#define DISPENSE_BUSY_TIMEOUT_MS  90000

static bool dispenser_mark_busy_if_idle(void)
{
    bool acquired = false;
    taskENTER_CRITICAL(&s_dispense_state_mux);
    if (!s_dispense_busy) {
        s_dispense_busy = true;
        s_dispense_busy_since = xTaskGetTickCount();
        acquired = true;
    }
    taskEXIT_CRITICAL(&s_dispense_state_mux);
    if (acquired) {
        /* Quiesce VL53 polling while we dispense — VL53 ops grab the
         * shared I2C mutex with retry, which delays the IR poll's
         * 2 kHz read cadence and makes pill detection misfire. */
        vl53l0x_multi_pause();
    }
    return acquired;
}

static void dispenser_clear_busy(void)
{
    bool was_busy = false;
    taskENTER_CRITICAL(&s_dispense_state_mux);
    was_busy = s_dispense_busy;
    s_dispense_busy = false;
    taskEXIT_CRITICAL(&s_dispense_state_mux);
    if (was_busy) {
        vl53l0x_multi_resume();
        /* Force a fresh VL53 read right now so the cartridge level on
         * web/Telegram reflects the post-dispense state without waiting
         * for the next 5 s polling cycle. */
        vl53l0x_request_refresh();
    }
}

static void send_telegram_photo_or_text(const char *msg)
{
    if (!msg || !msg[0]) return;
    if (!wifi_sta_connected()) {
        offline_sync_queue_telegram_text(msg);
        return;
    }

    /* Camera is lazy-init now — fire it up if this is the first capture
     * since boot. Skip the photo if init fails (broken ribbon, etc.) and
     * fall through to plain text so the user still gets the notification. */
    if (camera_ensure_initialized() != ESP_OK) {
        telegram_send_text(msg);
        return;
    }

    /* Mark a client so camera_task wakes up and encodes a fresh frame
     * (idle-skip optimisation otherwise leaves the buffer stale). */
    jpeg_enc_client_added();
    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    esp_err_t got = jpeg_enc_get_frame(&jpg_buf, &jpg_len, 3000);
    jpeg_enc_client_removed();
    if (got == ESP_OK) {
        uint8_t *copy_buf = (uint8_t *)malloc(jpg_len);
        if (copy_buf) {
            memcpy(copy_buf, jpg_buf, jpg_len);
            telegram_send_photo_with_text(copy_buf, jpg_len, msg);
        } else {
            telegram_send_text(msg);
        }
        jpeg_enc_release_frame();
    } else {
        telegram_send_text(msg);
    }
}

static bool telegram_lang_is_th(void)
{
    return telegram_get_language() == TELEGRAM_LANG_TH;
}

static const char *telegram_slot_label(int slot_idx)
{
    if (slot_idx < 0 || slot_idx >= 7) return "-";
    return telegram_lang_is_th() ? TG_SLOT_LABELS_TH[slot_idx] : SLOT_LABELS[slot_idx];
}

static const char *telegram_unknown_name(void)
{
    return telegram_lang_is_th() ? "ไม่ได้ตั้งชื่อ" : "Unknown";
}

static bool dispenser_slot_index_valid(int slot_idx)
{
    return slot_idx >= 0 && slot_idx < 7;
}

static void build_slot_med_names(int slot_idx, char *buf, size_t buf_len, bool only_in_stock)
{
    if (!buf || buf_len == 0) return;
    buf[0] = '\0';

    const netpie_shadow_t *sh = netpie_get_shadow();
    if (!sh || slot_idx < 0 || slot_idx >= 7) return;

    for (int i = 0; i < DISPENSER_MED_COUNT; i++) {
        if (!((sh->med[i].slots >> slot_idx) & 1)) continue;
        if (only_in_stock && sh->med[i].count <= 0) continue;

        const char *name = sh->med[i].name[0] ? sh->med[i].name : "Unknown";
        if (buf[0] != '\0') strncat(buf, ", ", buf_len - strlen(buf) - 1);
        strncat(buf, name, buf_len - strlen(buf) - 1);
    }
}

static void append_med_name(char *buf, size_t buf_len, const char *name)
{
    if (!buf || buf_len == 0 || !name || !name[0]) return;
    if (buf[0] != '\0') strncat(buf, ", ", buf_len - strlen(buf) - 1);
    strncat(buf, name, buf_len - strlen(buf) - 1);
}

static void reset_low_stock_alert_if_restocked(int med_idx, int current_count)
{
    if (med_idx < 0 || med_idx >= DISPENSER_MED_COUNT) return;
    if (current_count > 2) {
        s_low_stock_alert_sent[med_idx] = false;
    }
}

static void send_low_stock_alert(int med_idx, int current_count, const char *reason_th, const char *reason_en, bool force)
{
    const netpie_shadow_t *sh = netpie_get_shadow();
    if (!sh || med_idx < 0 || med_idx >= DISPENSER_MED_COUNT) return;

    // Skip the alert if none of this med's assigned meal slots have a real
    // time set — silent slots never dispense, so warning the user about
    // their stock just adds noise.
    {
        uint8_t mask = sh->med[med_idx].slots;
        bool any_active = false;
        for (int s = 0; s < 7 && !any_active; s++) {
            if (!((mask >> s) & 0x01)) continue;
            int hh, mm;
            if (parse_hhmm(sh->slot_time[s], &hh, &mm)) any_active = true;
        }
        if (!any_active) return;
    }

    if (!force) {
        if (current_count > 2) {
            s_low_stock_alert_sent[med_idx] = false;
            return;
        }
        if (s_low_stock_alert_sent[med_idx]) return;
    }

    char time_str[16] = "--:--";
    ds3231_get_time_str(time_str, sizeof(time_str));

    const char *med_name = sh->med[med_idx].name[0] ? sh->med[med_idx].name : telegram_unknown_name();
    char msg[384];
    if (telegram_lang_is_th()) {
        snprintf(msg, sizeof(msg),
                 "⚠️ แจ้งเตือนเติมยา\nเวลา: %s\nตลับยา: %d (%s)\nคงเหลือ: %d เม็ด\n%s",
                 time_str, med_idx + 1, med_name, current_count,
                 (reason_th && reason_th[0]) ? reason_th : "กรุณาเติมยาเพื่อให้เครื่องพร้อมใช้งานครั้งถัดไป");
    } else {
        snprintf(msg, sizeof(msg),
                 "⚠️ Refill reminder\nTime: %s\nCartridge: %d (%s)\nRemaining: %d pills\n%s",
                 time_str, med_idx + 1, med_name, current_count,
                 (reason_en && reason_en[0]) ? reason_en : "Please refill this medicine so the dispenser is ready for the next dose.");
    }
    telegram_send_text(msg);
    s_low_stock_alert_sent[med_idx] = true;
}

static void send_dispense_result_summary(int slot_idx,
                                         const char *dispensed_meds,
                                         const char *empty_meds,
                                         const char *missed_meds)
{
    if (!dispenser_slot_index_valid(slot_idx)) {
        ESP_LOGE(TAG, "Invalid slot index for dispense summary: %d", slot_idx);
        return;
    }

    const netpie_shadow_t *sh = netpie_get_shadow();
    if (!sh) {
        ESP_LOGE(TAG, "Shadow is unavailable while building dispense summary");
        return;
    }

    char time_str[16] = "--:--:--";
    ds3231_get_time_str(time_str, sizeof(time_str));

    bool has_dispensed = dispensed_meds && dispensed_meds[0];
    bool has_empty = empty_meds && empty_meds[0];
    bool has_missed = missed_meds && missed_meds[0];

    char *msg = (char *)calloc(1, 768);
    char *detail = (char *)calloc(1, 256);
    if (!msg || !detail) {
        ESP_LOGE(TAG, "Failed to allocate summary buffers");
        free(msg);
        free(detail);
        return;
    }

    const char *event_name = "Dispensed";
    const char *meds_field = (has_dispensed ? dispensed_meds :
                             (has_empty ? empty_meds :
                             (has_missed ? missed_meds : "-")));

    if (has_dispensed) {
        // Always ✅ when at least one pill was actually dispensed (matches
        // the success audio). Refill/check warnings are appended on extra
        // lines only when applicable, so a clean dispense reads cleanly
        // and a partial dispense still gets the success header.
        if (telegram_lang_is_th()) {
            int written = snprintf(msg, 768,
                     "✅ ยืนยันการรับยาแล้ว\nเวลา: %s\nมื้อ: %s (%s)\nยาที่จ่ายออกมา: %s",
                     time_str, telegram_slot_label(slot_idx),
                     sh->slot_time[slot_idx], dispensed_meds);
            if (has_empty && written < 768) {
                written += snprintf(msg + written, 768 - written,
                         "\nยาที่ควรเติม: %s", empty_meds);
            }
            if (has_missed && written < 768) {
                snprintf(msg + written, 768 - written,
                         "\nยาที่ต้องตรวจสอบการจ่าย: %s", missed_meds);
            }
        } else {
            int written = snprintf(msg, 768,
                     "Medication confirmed\nTime: %s\nDose: %s (%s)\nDispensed: %s",
                     time_str, telegram_slot_label(slot_idx),
                     sh->slot_time[slot_idx], dispensed_meds);
            if (has_empty && written < 768) {
                written += snprintf(msg + written, 768 - written,
                         "\nRefill: %s", empty_meds);
            }
            if (has_missed && written < 768) {
                snprintf(msg + written, 768 - written,
                         "\nCheck dispenser: %s", missed_meds);
            }
        }
        snprintf(detail, 256, "Slot %d (%s) | Out: %s | Empty: %s | Check: %s",
                 slot_idx, SLOT_LABELS[slot_idx], dispensed_meds,
                 has_empty ? empty_meds : "-", has_missed ? missed_meds : "-");
        event_name = (has_empty || has_missed) ? "Partial Dispense" : "Dispensed";
    } else {
        if (telegram_lang_is_th()) {
            snprintf(msg, 768,
                     "⚠️ มีการยืนยันรับยาแล้ว แต่ไม่พบยาจ่ายออกมา\nเวลา: %s\nมื้อ: %s (%s)\nยาที่ควรเติม: %s\nยาที่ต้องตรวจสอบการจ่าย: %s\nโปรดตรวจสอบเครื่องหรือเติมยา",
                     time_str, telegram_slot_label(slot_idx),
                     sh->slot_time[slot_idx],
                     has_empty ? empty_meds : "-",
                     has_missed ? missed_meds : "-");
        } else {
            snprintf(msg, 768,
                     "Medication confirmed, but no pill was dispensed\nTime: %s\nDose: %s (%s)\nRefill: %s\nCheck dispenser: %s",
                     time_str, telegram_slot_label(slot_idx),
                     sh->slot_time[slot_idx],
                     has_empty ? empty_meds : "-",
                     has_missed ? missed_meds : "-");
        }
        snprintf(detail, 256, "Slot %d (%s) | Out: none | Empty: %s | Check: %s",
                 slot_idx, SLOT_LABELS[slot_idx],
                 has_empty ? empty_meds : "-", has_missed ? missed_meds : "-");
        event_name = "Not Dispensed";
    }

    send_telegram_photo_or_text(msg);
    google_sheets_log(event_name, meds_field, detail);
    free(msg);
    free(detail);
}

static void send_stock_adjust_audit(int med_idx, int from_count, int to_count)
{
    const netpie_shadow_t *sh = netpie_get_shadow();
    if (!sh || med_idx < 0 || med_idx >= DISPENSER_MED_COUNT) return;

    /* VL53-sync audit rows ('V') removed 2026-05-12 per user request —
     * the dispense history should only carry actual dispense outcomes
     * (success / missed / no-stock), not background sensor reconciliation.
     * The Telegram alert below still fires so the user knows a stock
     * change was detected. */

    char time_str[16] = "--:--";
    ds3231_get_time_str(time_str, sizeof(time_str));

    char med_name[64];
    snprintf(med_name, sizeof(med_name), "%s",
             sh->med[med_idx].name[0] ? sh->med[med_idx].name : telegram_unknown_name());

    char msg[384];
    if (telegram_lang_is_th()) {
        snprintf(msg, sizeof(msg),
                 "มีการปรับจำนวนยาในหน้า Setup\nเวลา: %s\nโมดูล: %d (%s)\nจำนวน: %d -> %d (%+d)",
                 time_str, med_idx + 1, med_name, from_count, to_count, to_count - from_count);
    } else {
        snprintf(msg, sizeof(msg),
                 "Setup stock adjusted\nTime: %s\nModule: %d (%s)\nCount: %d -> %d (%+d)",
                 time_str, med_idx + 1, med_name, from_count, to_count, to_count - from_count);
    }
    send_telegram_photo_or_text(msg);

}

static void flush_pending_stock_audits(TickType_t now_ticks)
{
    for (int i = 0; i < DISPENSER_MED_COUNT; ++i) {
        bool should_send = false;
        int from_count = 0;
        int to_count = 0;

        taskENTER_CRITICAL(&s_stock_audit_mux);
        if (s_stock_audit[i].pending &&
            ((now_ticks - s_stock_audit[i].last_change_tick) * portTICK_PERIOD_MS >= STOCK_AUDIT_IDLE_MS)) {
            should_send = true;
            from_count = s_stock_audit[i].start_count;
            to_count = s_stock_audit[i].last_count;
            s_stock_audit[i].pending = false;
        }
        taskEXIT_CRITICAL(&s_stock_audit_mux);

        if (should_send && from_count != to_count) {
            send_stock_adjust_audit(i, from_count, to_count);
            reset_low_stock_alert_if_restocked(i, to_count);
        }
    }
}

/* ── Execute Dispense Logic (extracted from task) ── */
static void execute_dispense(int slot_idx)
{
    if (!dispenser_slot_index_valid(slot_idx)) {
        ESP_LOGE(TAG, "Invalid slot index for execute_dispense: %d", slot_idx);
        dispenser_clear_busy();
        return;
    }

    if (!s_dispense_mutex) {
        ESP_LOGE(TAG, "Dispense mutex is not initialized");
        dispenser_clear_busy();
        return;
    }

    if (xSemaphoreTake(s_dispense_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take dispense mutex");
        dispenser_clear_busy();
        return;
    }

    const netpie_shadow_t *sh = netpie_get_shadow();
    if (!sh) {
        ESP_LOGE(TAG, "Shadow is unavailable while dispensing");
        xSemaphoreGive(s_dispense_mutex);
        dispenser_clear_busy();
        return;
    }

    enum { MED_LIST_BUF_LEN = 256 };
    char *dispensed_meds = (char *)calloc(1, MED_LIST_BUF_LEN);
    char *empty_meds = (char *)calloc(1, MED_LIST_BUF_LEN);
    char *missed_meds = (char *)calloc(1, MED_LIST_BUF_LEN);
    if (!dispensed_meds || !empty_meds || !missed_meds) {
        ESP_LOGE(TAG, "Failed to allocate dispense result buffers");
        free(dispensed_meds);
        free(empty_meds);
        free(missed_meds);
        xSemaphoreGive(s_dispense_mutex);
        dispenser_clear_busy();
        return;
    }

    for (int i = 0; i < DISPENSER_MED_COUNT; i++) {
        if (!((sh->med[i].slots >> slot_idx) & 1)) continue;
        if (sh->med[i].count <= 0) {
            ESP_LOGW(TAG, "  med%d (%s) empty!", i+1, sh->med[i].name);
            append_med_name(empty_meds, MED_LIST_BUF_LEN,
                            sh->med[i].name[0] ? sh->med[i].name : telegram_unknown_name());
            /* Log a no-stock skip entry — count stays at 0 (allowed for
             * 'N' source). Lets the /log readout show "ยาหมดไม่จ่าย" for
             * the affected med and slot. */
            dispenser_audit_log(i, 0, 0, 'N');
            continue;
        }

        ESP_LOGI(TAG, "  💊 Dispensing med%d (%s) ch%d", i+1, sh->med[i].name, i);

        bool pill_detected = false;
        ESP_LOGI(TAG, "      ▶️ [IR SENSOR] Start monitoring pill drop for med%d...", i+1);

        // Warmup phase before issuing servo command: prime the
        // PCF8574 bus + let IR sensor stabilize. Without this the
        // very first cycle of a dispense sometimes returned stale
        // 0xFF (no pill) for a fast-dropping pill, making the first
        // pill detection flaky.
        for (int w = 0; w < 8; w++) {
            uint8_t junk = 0xFF;
            (void)pcf8574_read(&junk);
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        // Continuous IR polling: from servo work command through home
        // and 2 s past home. pcf8574_read takes ~3-5 ms via I2C; we
        // yield (vTaskDelay 1 tick) on EVERY iteration so the UI/touch
        // task on the same I2C bus gets fair access. Earlier we yielded
        // only every 8 reads, which held the bus for ~30-40 ms at a
        // stretch and froze the screen during the 7.5 s IR window
        // (visible during return-pill / scheduled-dispense). Yielding
        // every read still gives ~100 Hz IR sampling — well above the
        // ~20 Hz needed to catch a falling pill (~50 ms beam-break).
        const uint32_t WORK_MS  = 1500;
        const uint32_t HOME_MS  = 4000;
        const uint32_t POST_MS  = 2000;
        /* Pulse-width detection for a real pill drop, replacing the old
         * "sustained LOW = pill detected" rule that false-positives on
         * EMI from servo PWM AND on the cup arm sweeping across the IR
         * beam during rotation.
         *
         * A real pill blocks the beam for ~30..200 ms (gravity drop
         * through a ~5-10 cm chute). EMI bursts are sub-millisecond per
         * PWM edge — even sustained supply sag from a long ramp keeps
         * the beam LOW for the whole ramp (>500 ms). The cup arm, if
         * it crosses the beam, also keeps the IR LOW for several
         * hundred ms.
         *
         * So we accept a pill only when the LOW pulse meets BOTH a
         * minimum (≥3 samples, ~30 ms — rejects single-tick EMI) and
         * a maximum (≤25 samples, ~250 ms — rejects sustained EMI and
         * arm sweeps). The pulse must also END with a debounced HIGH
         * (≥3 HIGH samples in a row), proving the beam actually
         * cleared — not just dropped LOW and stayed. */
        const int IR_DEBOUNCE_LOW_SAMPLES  = 3;   /* ≥30 ms LOW = beam armed */
        const int IR_DEBOUNCE_HIGH_SAMPLES = 3;   /* ≥30 ms HIGH = beam clear */
        const int IR_MAX_LOW_SAMPLES       = 25;  /* >250 ms LOW = not a pill */
        int consec_low = 0;
        int consec_high = 0;
        int armed_low_count = 0;  /* total LOW samples since beam-armed */
        bool beam_armed = false;
        uint32_t loop_start = esp_log_timestamp();
        esp_err_t go_work_err = pca9685_go_work(i);
        if (go_work_err != ESP_OK) {
            // Servo command failed — don't waste 7.5 s polling IR for a
            // pill that can't drop. Log + skip this medication so the
            // scheduled dispense reports the missed slot instead of
            // silently passing through with no servo motion.
            ESP_LOGE(TAG, "      ✗ pca9685_go_work(med%d) failed: %s — skipping IR window",
                     i + 1, esp_err_to_name(go_work_err));
            continue;
        }
        bool home_issued = false;
        while (1) {
            uint32_t elapsed = esp_log_timestamp() - loop_start;
            if (!home_issued && elapsed >= WORK_MS) {
                esp_err_t go_home_err = pca9685_go_home(i);
                if (go_home_err != ESP_OK) {
                    ESP_LOGE(TAG, "      ✗ pca9685_go_home(med%d) failed: %s — pill cup may be left rotated",
                             i + 1, esp_err_to_name(go_home_err));
                }
                home_issued = true;
            }
            if (elapsed >= (WORK_MS + HOME_MS + POST_MS)) break;

            uint8_t ir_val = 0xFF;
            if (pcf8574_read(&ir_val) == ESP_OK) {
                bool beam_blocked = ((ir_val & (1 << i)) == 0);
                if (beam_blocked) {
                    consec_high = 0;
                    consec_low++;
                    if (beam_armed) armed_low_count++;
                    if (!beam_armed && consec_low >= IR_DEBOUNCE_LOW_SAMPLES) {
                        beam_armed = true;
                        armed_low_count = consec_low;
                        ESP_LOGI(TAG, "      >> IR ch%d beam ARMED at %lu ms (LOW debounced)",
                                 i + 1, (unsigned long)elapsed);
                    }
                    /* Stuck LOW (cup arm in beam, EMI supply sag, or
                     * stuck signal) — disarm and ignore. Real pill
                     * never holds beam this long. */
                    if (beam_armed && armed_low_count > IR_MAX_LOW_SAMPLES) {
                        ESP_LOGW(TAG, "      ⚠ IR ch%d beam LOW too long (%d samples >%d) — "
                                      "disarming (likely arm sweep or EMI, not a pill)",
                                 i + 1, armed_low_count, IR_MAX_LOW_SAMPLES);
                        beam_armed = false;
                        armed_low_count = 0;
                        consec_low = 0;
                    }
                } else {
                    consec_low = 0;
                    if (beam_armed) {
                        consec_high++;
                        if (consec_high >= IR_DEBOUNCE_HIGH_SAMPLES) {
                            /* LOW pulse cleared cleanly within the
                             * allowed width window → real pill passage. */
                            if (!pill_detected) {
                                ESP_LOGI(TAG, "      ✓ IR ch%d pill CONFIRMED at %lu ms "
                                              "(LOW pulse %d samples, then clean HIGH)",
                                         i + 1, (unsigned long)elapsed, armed_low_count);
                            }
                            pill_detected = true;
                            beam_armed = false;
                            armed_low_count = 0;
                            consec_high = 0;
                        }
                    } else {
                        consec_high = 0;
                    }
                }
            }
            vTaskDelay(1);
        }
        /* End-of-window: if beam is still in an armed-but-not-cleared
         * state with a reasonable pulse width, count it. Catches the
         * edge case where the pill is still mid-air at window end. */
        if (!pill_detected && beam_armed &&
            armed_low_count >= IR_DEBOUNCE_LOW_SAMPLES &&
            armed_low_count <= IR_MAX_LOW_SAMPLES) {
            ESP_LOGI(TAG, "      ✓ IR ch%d pill counted at window-end (armed, %d samples LOW)",
                     i + 1, armed_low_count);
            pill_detected = true;
        }

        if (!pill_detected) {
            ESP_LOGW(TAG, "      ❌ [IR SENSOR] MISSED! No pill detected dropping for med%d", i+1);
            append_med_name(missed_meds, MED_LIST_BUF_LEN,
                            sh->med[i].name[0] ? sh->med[i].name : telegram_unknown_name());
        } else {
            ESP_LOGI(TAG, "      ✅ [IR SENSOR] SUCCESS! Pill drop confirmed for med%d", i+1);
            append_med_name(dispensed_meds, MED_LIST_BUF_LEN,
                            sh->med[i].name[0] ? sh->med[i].name : telegram_unknown_name());
        }

        vTaskDelay(pdMS_TO_TICKS(500));

        if (pill_detected) {
            int old_count = sh->med[i].count;
            int new_count = old_count - 1;
            if (new_count < 0) new_count = 0;
            netpie_shadow_update_count(i + 1, new_count);
            dispenser_audit_log(i, old_count, new_count, 'S');
            send_low_stock_alert(i, new_count,
                                 "ยาใกล้หมด เหลือ 2 เม็ดสุดท้ายหรือน้อยกว่า",
                                 "Medicine is running low. Two pills or fewer remain.",
                                 false);
        } else {
            /* Scheduled dose ran but IR saw nothing fall — cartridge is
             * empty in reality. Force shadow.count = 0 so the on-screen
             * stock matches reality (manual dispense already does this at
             * line ~1621). Also resync the UI snapshot so a stray Back
             * tap from the meds-detail page can't revert it. */
            int old_count = sh->med[i].count;
            if (old_count != 0) {
                netpie_shadow_update_count(i + 1, 0);
                dispenser_audit_log(i, old_count, 0, 'S');
                extern void ui_setup_meds_resync_backup_count(int, int);
                ui_setup_meds_resync_backup_count(i, 0);
            }
            send_low_stock_alert(i, 0,
                                 "มีการยืนยันรับยาแต่ไม่พบยาออกจากช่อง ระบบตั้งยาคงเหลือเป็น 0 แล้ว กรุณาเติมยา",
                                 "Dose was confirmed but no pill was detected. Stock was set to 0. Please refill.",
                                 true);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    xSemaphoreGive(s_dispense_mutex);
    dispenser_clear_busy();

    // Result sound for the scheduled dose: at least one pill dropped =
    // จ่ายยาสำเร็จ; nothing dropped = ไม่พบยา.
    {
        extern int g_snd_disp_th, g_snd_disp_en, g_snd_nomeds_th, g_snd_nomeds_en;
        bool any_dispensed = (dispensed_meds && dispensed_meds[0] != '\0');
        bool is_th = telegram_lang_is_th();
        if (any_dispensed) {
            dfplayer_play_track(is_th ? g_snd_disp_th : g_snd_disp_en);
        } else {
            dfplayer_play_track(is_th ? g_snd_nomeds_th : g_snd_nomeds_en);
        }
    }

    send_dispense_result_summary(slot_idx, dispensed_meds, empty_meds, missed_meds);
    free(dispensed_meds);
    free(empty_meds);
    free(missed_meds);
}

/* ── Dispenser Task ── */
static void dispenser_task(void *arg)
{
    ESP_LOGI(TAG, "Dispenser scheduler task started");
    uint32_t last_rtc_check = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(250)); // Fast loop to catch UI commands instantly
        uint32_t now_ticks = xTaskGetTickCount();
        uint32_t now_ms = now_ticks * portTICK_PERIOD_MS;

        flush_pending_stock_audits(now_ticks);

        // Watchdog: if s_dispense_busy stayed true longer than the
        // timeout, something deadlocked (servo bind, I2C wedge, task
        // crashed before clearing). Force-clear so the next dispense
        // can proceed instead of permanently bricking the dispenser.
        // Read the (busy, since) pair atomically to avoid a torn read
        // where busy==true but since==0 trips an instant timeout.
        bool busy_now;
        TickType_t busy_since;
        taskENTER_CRITICAL(&s_dispense_state_mux);
        busy_now = s_dispense_busy;
        busy_since = s_dispense_busy_since;
        taskEXIT_CRITICAL(&s_dispense_state_mux);
        if (busy_now &&
            ((now_ticks - busy_since) * portTICK_PERIOD_MS) > DISPENSE_BUSY_TIMEOUT_MS) {
            ESP_LOGE(TAG, "Dispense busy >%dms — forcing clear (servo hang or task crash)",
                     DISPENSE_BUSY_TIMEOUT_MS);
            dispenser_clear_busy();
            // Try to send servos home so a half-rotated cup doesn't sit
            // in the work position blocking the next dispense.
            for (int i = 0; i < DISPENSER_MED_COUNT; ++i) {
                pca9685_go_home(i);
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }

        // Check timeout logic
        if (s_waiting_confirm) {
            uint32_t elapsed_sec = (now_ticks - s_wait_start_ticks) * portTICK_PERIOD_MS / 1000;
            if (elapsed_sec > 900) { // 15 mins
                ESP_LOGW(TAG, "User missed medication for slot %d", s_pending_slot_idx);
                if (dispenser_slot_index_valid(s_pending_slot_idx)) {
                    g_missed_slots_mask |= (1 << s_pending_slot_idx);
                    /* Audit: one "missed dose" row per med assigned to
                     * the slot that just timed out. Count is unchanged
                     * (the dose was never dispensed) so we pass the
                     * current count for both from/to — 'L' source is
                     * exempt from the no-op suppression in
                     * dispenser_audit_log. */
                    const netpie_shadow_t *sh = netpie_get_shadow();
                    for (int i = 0; i < DISPENSER_MED_COUNT; ++i) {
                        if ((sh->med[i].slots >> s_pending_slot_idx) & 1) {
                            dispenser_audit_log(i, sh->med[i].count,
                                                sh->med[i].count, 'L');
                        }
                    }
                }
                dispenser_skip_meds();
            }
            continue; // Suspend RTC trigger checks while waiting
        }

        // Check dispense logic
        if (s_dispense_approved) {
            if (dispenser_mark_busy_if_idle()) {
                /* Snapshot the slot index inside the same atomic burst
                 * as the approved flag so a concurrent dispenser_skip_meds
                 * (which writes -1) can't leak a torn read here.
                 * execute_dispense itself uses `slot_idx >> ...` so an
                 * out-of-range value would be undefined behavior. */
                int slot_to_run;
                portENTER_CRITICAL(&s_dispense_state_mux);
                s_dispense_approved = false;
                slot_to_run = s_pending_slot_idx;
                s_pending_slot_idx = -1;
                portEXIT_CRITICAL(&s_dispense_state_mux);
                if (dispenser_slot_index_valid(slot_to_run)) {
                    execute_dispense(slot_to_run);
                } else {
                    ESP_LOGW(TAG, "Approved dispense had invalid slot_idx=%d — skipped",
                             slot_to_run);
                    dispenser_clear_busy();
                }
            }
        }

        // Time check logic (Every 10 secs)
        if (now_ms - last_rtc_check > 10000) {
            last_rtc_check = now_ms;

            static char s_current_date[32] = "";
            char dt_str[32] = "";
            if (ds3231_get_date_str(dt_str, sizeof(dt_str)) == ESP_OK && dt_str[0] != '\0') {
                if (s_current_date[0] == '\0') {
                    strncpy(s_current_date, dt_str, sizeof(s_current_date));
                } else if (strcmp(dt_str, s_current_date) != 0) {
                    // Day changed, reset missed slots mask
                    strncpy(s_current_date, dt_str, sizeof(s_current_date));
                    g_missed_slots_mask = 0;
                }
            }

            char t_str[16] = "";
            ds3231_get_time_str(t_str, sizeof(t_str));
            if (strlen(t_str) < 5) continue;

            int cur_h, cur_m;
            if (!parse_hhmm(t_str, &cur_h, &cur_m)) continue;

            char cur_hhmm[6];
            snprintf(cur_hhmm, sizeof(cur_hhmm), "%02d:%02d", cur_h, cur_m);

            update_next_dose_str(cur_h, cur_m);

            const netpie_shadow_t *sh = netpie_get_shadow();
            if (!sh->loaded || !sh->enabled) continue;
            if (s_emergency_stop) continue;  // Skip slot eval entirely while stopped
            if (dispenser_in_quiet_hours(cur_h, cur_m)) {
                // Within user-configured quiet window — auto-skip slot
                // triggers so the elderly user isn't woken up by an
                // alarm at 06:00 if they want quiet until 07:00.
                continue;
            }

            for (int s = 0; s < 7; s++) {
                int th, tm;
                if (!parse_hhmm(sh->slot_time[s], &th, &tm)) continue;

                // Match any minute in [slot, slot+SLOT_GRACE_MIN]. The
                // task wakes every ~30 s, so the exact-minute match is
                // fragile — if a previous dispense holds the task busy
                // for >60 s the slot would silently be missed. Grace
                // window absorbs that. Per-slot 12-hour fire guard
                // below prevents a wide grace from re-firing.
                int slot_total = th * 60 + tm;
                int cur_total  = cur_h * 60 + cur_m;
                int delta = cur_total - slot_total;
                // Wrap: 23:59 vs 00:00 should be -1 not 1439. Fold any
                // delta < -720 / > 720 around the day.
                if (delta >  720) delta -= 1440;
                if (delta < -720) delta += 1440;
                if (delta < 0 || delta > SLOT_GRACE_MIN) continue;

                // Per-slot 12-hour refire guard: regardless of grace,
                // a slot may not refire within 12 h of its last fire.
                time_t now_epoch = time(NULL);
                if (s_slot_last_fire[s] != 0 &&
                    (now_epoch - s_slot_last_fire[s]) < SLOT_REFIRE_GUARD_SEC) {
                    continue;
                }

                bool has_assigned = false;
                bool has_stock = false;
                char empty_meds[128] = "";

                for (int i = 0; i < DISPENSER_MED_COUNT; i++) {
                    if ((sh->med[i].slots >> s) & 1) {
                        has_assigned = true;
                        if (sh->med[i].count > 0) {
                            has_stock = true; 
                        } else {
                            if (strlen(empty_meds) > 0) strcat(empty_meds, ", ");
                            strncat(empty_meds, sh->med[i].name[0] ? sh->med[i].name : "Unknown", sizeof(empty_meds) - strlen(empty_meds) - 1);
                        }
                    }
                }
                
                if (!has_assigned) continue;

                if (strcmp(cur_hhmm, s_last_triggered) == 0) continue;
                snprintf(s_last_triggered, sizeof(s_last_triggered), "%s", cur_hhmm);
                s_slot_last_fire[s] = now_epoch;
                /* Persist refire guards so a reboot just after a fired
                 * slot doesn't re-trigger the Confirm popup on next
                 * boot (user observed "ตอนบูสเครื่องเสร็จมันเด้งมาให้
                 * กดจ่ายยา"). NVS write is cheap; only happens on
                 * actual slot fires. */
                {
                    nvs_handle_t h;
                    if (nvs_open("dispenser", NVS_READWRITE, &h) == ESP_OK) {
                        nvs_set_blob(h, "slot_lastfire",
                                     s_slot_last_fire, sizeof(s_slot_last_fire));
                        nvs_commit(h);
                        nvs_close(h);
                    }
                }

                if (strlen(empty_meds) > 0) {
                     char msg[256];
                     if (telegram_lang_is_th()) {
                         snprintf(msg, sizeof(msg), "⚠️ แจ้งเตือนยาหมด\nมื้อ: %s\nโมดูลที่ต้องเติม: %s",
                                  telegram_slot_label(s), empty_meds);
                     } else {
                         snprintf(msg, sizeof(msg), "Out-of-stock alert\nDose: %s\nModules to refill: %s",
                                  telegram_slot_label(s), empty_meds);
                     }
                     telegram_send_text(msg);
                }

                if (!has_stock) {
                     ESP_LOGW(TAG, "Triggered slot %s, but fully out of stock. Telegram alert sent.", SLOT_LABELS[s]);
                     s_empty_stock_warning = true;
                } else {
                     s_empty_stock_warning = false;
                }

                ESP_LOGI(TAG, "⏰ Slot %d (%s) triggered at %s. Waiting for User Confirmation...", s, SLOT_LABELS[s], cur_hhmm);

                s_waiting_confirm = true;
                s_wait_start_ticks = now_ticks;
                s_pending_slot_idx = s;
                /* Pause VL53 polling for the entire 15-min user-tap
                 * window so the shared I2C bus stays quiet — touch reads
                 * miss less, and the user's tap registers immediately
                 * (otherwise VL53 every-5-s polls would occasionally
                 * stall the touch driver enough that taps got dropped,
                 * and the system fired the "didn't tap in time" alert
                 * even though the user actually tapped). */
                vl53l0x_multi_pause();
                break; // Stop evaluating other slots
            }

            // ── Pre-alerts: warn user at 30, 15, and 5 minutes before each dose ──────────
            for (int s = 0; s < 7; s++) {
                int th, tm;
                if (!parse_hhmm(sh->slot_time[s], &th, &tm)) continue;

                // Check if this slot has any medicine assigned
                bool has_assigned = false;
                for (int i = 0; i < DISPENSER_MED_COUNT; i++) {
                    if ((sh->med[i].slots >> s) & 1) {
                        has_assigned = true;
                        break;
                    }
                }
                if (!has_assigned) continue;

                // Check: difference between current time and target slot time
                int slot_total = th * 60 + tm;
                int cur_total  = cur_h * 60 + cur_m;
                int diff = slot_total - cur_total;
                
                if (diff == 30 || diff == 15 || diff == 5) {
                    // De-duplicate using a separate last_prealert key
                    static char s_last_prealert[16] = "";
                    char prealert_key[12];
                    snprintf(prealert_key, sizeof(prealert_key), "%s-%d", cur_hhmm, s);
                    if (strcmp(prealert_key, s_last_prealert) == 0) continue;
                    snprintf(s_last_prealert, sizeof(s_last_prealert), "%s", prealert_key);

                    // Send Telegram Alert
                    char pre_msg[256];
                    if (telegram_lang_is_th()) {
                        snprintf(pre_msg, sizeof(pre_msg),
                                 "🔔 แจ้งเตือนล่วงหน้า %d นาที\nมื้อ: %s (%s)\nเตรียมรับยาได้เลย",
                                 diff, telegram_slot_label(s), sh->slot_time[s]);
                    } else {
                        snprintf(pre_msg, sizeof(pre_msg),
                                 "Upcoming medication in %d minutes\nDose: %s (%s)\nPlease get ready.",
                                 diff, telegram_slot_label(s), sh->slot_time[s]);
                    }
                    telegram_send_text(pre_msg);
                    ESP_LOGI(TAG, "Pre-alert (%d mins) sent for slot %d (%s)", diff, s, SLOT_LABELS[s]);
                    
                    // Stop current track (if any) and play the next alert
                    // Volume logic is gracefully handled dynamically based on track internally by dfplayer_play_track()
                    dfplayer_stop();
                    vTaskDelay(pdMS_TO_TICKS(150));
                    
                    // Plays track based on physical SD card index
                    // 1 = General Alarm, 2 = 30 min, 3 = 15 min, 4 = 5 min
                    if (diff == 30) {
                        dfplayer_play_track(2);
                    } else if (diff == 15) {
                        dfplayer_play_track(3);
                    } else if (diff == 5) {
                        dfplayer_play_track(4);
                    }
                }
            }

        } // end if (now_ms - last_rtc_check)
    } // end while(true)
} // end dispenser_task

/* ─────────────────────────────────────────────────────────────
   Public API
───────────────────────────────────────────────────────────── */

void dispenser_scheduler_start(void)
{
    if (!s_dispense_mutex) {
        s_dispense_mutex = xSemaphoreCreateMutex();
        if (!s_dispense_mutex) {
            ESP_LOGE(TAG, "Failed to create dispense mutex");
            return;
        }
    }
    dispenser_emergency_load_nvs();
    quiet_hours_load_nvs();
    dispenser_ir_load_nvs();
    dispenser_audit_load_nvs();
    if (s_emergency_stop) {
        ESP_LOGW(TAG, "Dispenser starting with emergency stop ACTIVE — "
                      "no dispense will fire until /resume or web Clear");
    }
    if (xTaskCreate(dispenser_task, "dispenser", DISPENSER_TASK_STACK_SIZE, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create dispenser scheduler task");
        if (s_dispense_mutex) {
            vSemaphoreDelete(s_dispense_mutex);
            s_dispense_mutex = NULL;
        }
        return;
    }
    ESP_LOGI(TAG, "Dispenser scheduler started");
}

void dispenser_get_next_dose_str(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) return;
    strncpy(buf, s_next_dose, buf_len - 1);
    buf[buf_len - 1] = '\0';
}

bool dispenser_is_waiting(void) { return s_waiting_confirm; }

void dispenser_reset_slot_refire_guard(int slot_idx)
{
    if (slot_idx < 0 || slot_idx >= 7) return;
    s_slot_last_fire[slot_idx] = 0;
    /* Also reset s_last_triggered so the per-minute dedup doesn't
     * block the new time if it happens to match the previous trigger
     * minute (e.g., previous test fired at 14:32, user re-sets to
     * 14:32 next day — without this, the cur_hhmm==s_last_triggered
     * check on line ~1121 would still suppress). */
    s_last_triggered[0] = '\0';
    ESP_LOGI(TAG, "Refire guard cleared for slot %d (slot_time changed)", slot_idx);
}
bool dispenser_is_empty_warning(void) { return s_empty_stock_warning; }

int dispenser_seconds_left(void) {
    if (!s_waiting_confirm) return 0;
    uint32_t elapsed = (xTaskGetTickCount() - s_wait_start_ticks) * portTICK_PERIOD_MS / 1000;
    return elapsed >= 900 ? 0 : (900 - elapsed);
}

int dispenser_waiting_slot(void) { return s_pending_slot_idx; }

void dispenser_confirm_meds(void) {
    // s_waiting_confirm / s_dispense_approved are read by the scheduler
    // task (see line ~983) and written here from the UI. Without the mux
    // a torn read could cause approval to be missed or a stale slot
    // index to be dispensed. Keep the critical section short — no
    // logging or I2C inside.
    portENTER_CRITICAL(&s_dispense_state_mux);
    bool was_waiting = s_waiting_confirm;
    if (was_waiting) {
        s_dispense_approved = true;
        s_waiting_confirm = false;
    }
    portEXIT_CRITICAL(&s_dispense_state_mux);
    if (was_waiting) {
        ESP_LOGI(TAG, "User CONFIRMED medication drop.");
        /* Balance the pause issued when waiting started.
         * dispenser_mark_busy_if_idle will pause VL53 again before the
         * dispense actually runs, so VL53 stays quiet end-to-end. */
        vl53l0x_multi_resume();
    }
}

void dispenser_skip_meds(void) {
    // Snapshot state under the mux, then do logging / Telegram /
    // Google-Sheets outside the critical section.
    portENTER_CRITICAL(&s_dispense_state_mux);
    bool was_waiting = s_waiting_confirm;
    int  slot_idx    = s_pending_slot_idx;
    if (was_waiting) {
        s_waiting_confirm = false;
        s_pending_slot_idx = -1;
    }
    portEXIT_CRITICAL(&s_dispense_state_mux);
    if (!was_waiting) return;

    /* Balance the pause issued when waiting started. Skip path doesn't
     * dispense, so VL53 just resumes its normal polling cadence. */
    vl53l0x_multi_resume();

    ESP_LOGI(TAG, "User SKIPPED medication drop.");
    if (dispenser_slot_index_valid(slot_idx)) {
        g_missed_slots_mask |= (1 << slot_idx);
    }

    char msg[512];
    if (telegram_lang_is_th()) {
        snprintf(msg, sizeof(msg),
                 "❌ ไม่มีผู้มารับยาภายใน 15 นาที\nมื้อ: %s\nระบบจึงไม่จ่ายยาออกมา",
                 telegram_slot_label(slot_idx));
    } else {
        snprintf(msg, sizeof(msg),
                 "Medication was not collected within 15 minutes.\nDose: %s\nThe dispenser skipped this round.",
                 telegram_slot_label(slot_idx));
    }

    send_telegram_photo_or_text(msg);

    // Log Skipped to Google Sheets
    char skipped_meds[256] = "";
    build_slot_med_names(slot_idx, skipped_meds, sizeof(skipped_meds), true);
    char detail_str[64];
    snprintf(detail_str, sizeof(detail_str), "Slot %d (%s)", slot_idx, SLOT_LABELS[slot_idx]);
    google_sheets_log("Skipped (Timeout)", strlen(skipped_meds) > 0 ? skipped_meds : "-", detail_str);
}

/* ── Manual Dispense Background Sequence ── */
typedef struct {
    int med_idx;
    int qty;
} manual_disp_args_t;

volatile int ui_manual_disp_status = 0; // 0=Idle, 1=Dropping, 2=Success, 3=Fail

static void manual_dispense_task(void *arg) {
    manual_disp_args_t *args = (manual_disp_args_t *)arg;
    int m_idx = args->med_idx;
    int qty = args->qty;
    free(args); // free the transient params

    if (!s_dispense_mutex) {
        ESP_LOGE(TAG, "Dispense mutex is not initialized");
        dispenser_clear_busy();
        ui_manual_disp_status = 3;
        vTaskDelete(NULL);
        return;
    }
    if (xSemaphoreTake(s_dispense_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take dispense mutex for manual dispense");
        dispenser_clear_busy();
        ui_manual_disp_status = 3;
        vTaskDelete(NULL);
        return;
    }

    ui_manual_disp_status = 1;
    dfplayer_play_track(32); // Track 32: Processing / Please wait
    ESP_LOGI(TAG, "Starting manual dispense: med%d, requested q:%d", m_idx + 1, qty);

    int actually_dropped = 0;
    bool eject_all = (qty == 100);
    bool forced_empty = false;
    int dead_ir_drops = 0;
    int consecutive_empty_cycles = 0;
    // Strict IR mode: any cycle where IR doesn't detect a pill stops
    // the dispense immediately. No retry, no servo-trust fallback for
    // modules that DO have IR enabled. For modules with no IR sensor
    // wired, disable IR via /tech/ir (m=N&v=0) so the loop runs in
    // servo-trust mode from the start.
    const int EMPTY_CONFIRM_CYCLES = 1;
    const int EMPTY_MIN_CYCLES_BEFORE_BAIL = 0;

    const netpie_shadow_t *sh = netpie_get_shadow();
    if (!sh) {
        ESP_LOGE(TAG, "Shadow not loaded — aborting manual dispense");
        ui_manual_disp_status = 3;
        xSemaphoreGive(s_dispense_mutex);
        dispenser_clear_busy();
        vTaskDelete(NULL);
        return;
    }

    /* Manual / return-pill flow is servo-trust ONLY (no IR check).
     * Per user spec: "การคืนยา+ir ใช้servoจ่ายไปเลย16ครั้ง โดย irใช้เช็คแค่
     * ตอนจ่ายยาตามมื้อ". IR sensor is still used during scheduled dispense
     * (run_dispense_slot_now path) — that path is unchanged. Rationale:
     * cartridge stock-out is now caught by VL53 polling + low-pill
     * Telegram alert, so IR's "no pill" detection is redundant for the
     * manual path which the user can visually verify themselves. */
    int loops = eject_all ? DISPENSER_MAX_PILLS : qty;
    char requested_str[16];
    snprintf(requested_str, sizeof(requested_str), "%s", eject_all ? "ALL" : "");
    if (!eject_all) snprintf(requested_str, sizeof(requested_str), "%d", qty);

    // Debounce: pill must be seen LOW for at least this many consecutive
    // polls to count. Bumped 1 → 3 after field reports of phantom pill
    // detections during return-pill (servo PWM 50 Hz coupling onto
    // PCF8574 input pins gives single-shot LOW spikes mid-poll). 3
    // consecutive LOW reads at ~100 Hz = 30 ms minimum dwell — still
    // catches real pills (beam-break ≥50 ms) but rejects glitches.
    const int IR_LOW_DEBOUNCE_SAMPLES = 3;
    // Settle time after servo returns home — let any pill in flight finish
    // falling past IR. Then sample the beam: if it's clear, the cartridge
    // is now ready for the next attempt; if it's STILL blocked, something
    // is jammed in the chute (we don't issue another servo cycle).
    const int IR_POST_HOME_SETTLE_MS  = 350;
    const int IR_POST_HOME_SAMPLES    = 8;
    // Per-cycle IR polling windows. Bumped from 1000/3000 ms after users
    // reported return-all stopping early on cartridges with slow-falling
    // pills — give every cycle a fuller chance to see a pill before the
    // empty-streak logic decides the cartridge is done.
    const uint32_t IR_WORK_WINDOW_MS  = 1500;
    const uint32_t IR_HOME_WINDOW_MS  = 4000;

    // Per-session IR availability — taken from the NVS-stored flag.
    // For modules without IR wired, disable via /tech/ir to put the
    // loop in servo-trust mode from the start. Strict IR mode otherwise.
    /* Force IR off for the manual path regardless of per-module config.
     * Scheduled dispense still uses s_ir_present[m_idx] in its own loop. */
    bool ir_use = false;

    // Warmup the PCF8574 bus before the first dispense cycle so a
    // fast-falling first pill isn't missed by a stale read.
    if (ir_use) {
        for (int w = 0; w < 8; w++) {
            uint8_t junk = 0xFF;
            (void)pcf8574_read(&junk);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    for (int i = 0; i < loops; i++) {
        ESP_LOGI(TAG, "  💊 Manual Drop %d/%d for med%d (ir_use=%d)",
                 i + 1, loops, m_idx + 1, ir_use ? 1 : 0);
        /* Edge-counted detection: each beam-block→clear transition is
         * one pill. Old logic was a boolean per cycle which capped each
         * servo rotation at 1 counted pill — even when multiple pills
         * physically dropped together (visible as IR LED flashes the
         * user reported but Telegram undercounted). */
        int  pills_this_cycle = 0;
        bool ir_alive = false;        // true if pcf8574_read ever returned ESP_OK
        int  ok_reads = 0;            // diagnostics
        int  fail_reads = 0;
        uint8_t ir_min = 0xFF;        // lowest byte ever observed
        uint8_t ir_last = 0xFF;
        int  consecutive_low  = 0;    // debounce counter (beam blocked)
        int  consecutive_high = 0;    // debounce counter (beam clear)
        bool beam_armed = false;      // true after a debounced LOW → next debounced HIGH = pill
        bool ir_present = ir_use;
        bool post_clear = true;

        // Unified continuous polling: servo work → home → 2s post.
        // Yield 1 tick after EVERY pcf8574_read so the touch/UI task
        // on the shared I²C bus isn't starved during the 7.5 s IR
        // window. Earlier we batched yields every 8 reads, but
        // pcf8574_read holds g_i2c_mutex for ~3-5 ms, so 8 back-to-back
        // reads kept the bus busy for ~30-40 ms — long enough to
        // freeze the screen during return-pill (manual_dispense_task).
        const uint32_t POST_HOME_EXTRA_MS = 2000;
        // Servo cycle for the manual / return-pill path. Log the WORK
        // command result loudly — if pca9685_go_work fails (I²C bus
        // wedged, PCA9685 not initialized) the servo never moves and
        // every subsequent IR poll just sees an empty chute. Earlier
        // we waited 7.5 s before reporting the failure, which made it
        // look like the firmware was hung when really the servo had
        // never been driven. Bail immediately on error.
        ESP_LOGI(TAG, "  → servo WORK med%d (home=%d work=%d)",
                 m_idx + 1,
                 g_servo[m_idx].home_angle, g_servo[m_idx].work_angle);
        esp_err_t err1 = pca9685_go_work(m_idx);
        if (err1 != ESP_OK) {
            ESP_LOGE(TAG, "  ✗ pca9685_go_work(med%d) failed: %s — aborting cycle",
                     m_idx + 1, esp_err_to_name(err1));
        }
        uint32_t loop_start = esp_log_timestamp();
        esp_err_t err2 = ESP_OK;
        bool home_issued = false;
        if (err1 != ESP_OK) {
            // Servo never moved — skip the IR window entirely. Falls
            // through to the err1 check below which logs + bails.
            pills_this_cycle = 0;
        } else if (ir_present) {
            /* Symmetric debounce: 3 consecutive LOW reads = beam armed,
             * 3 consecutive HIGH reads = pill counted + beam disarmed.
             * Same debounce on both edges rejects glitches in both
             * directions. */
            const int IR_HIGH_DEBOUNCE_SAMPLES = IR_LOW_DEBOUNCE_SAMPLES;
            while (1) {
                uint32_t elapsed = esp_log_timestamp() - loop_start;
                if (!home_issued && elapsed >= IR_WORK_WINDOW_MS) {
                    err2 = pca9685_go_home(m_idx);
                    home_issued = true;
                }
                if (elapsed >= (IR_WORK_WINDOW_MS + IR_HOME_WINDOW_MS + POST_HOME_EXTRA_MS)) break;

                uint8_t ir = 0xFF;
                esp_err_t r = pcf8574_read(&ir);
                if (r == ESP_OK) {
                    ir_alive = true;
                    ok_reads++;
                    if (ir < ir_min) ir_min = ir;
                    ir_last = ir;
                    bool beam_blocked = ((ir & (1 << m_idx)) == 0);
                    if (beam_blocked) {
                        consecutive_high = 0;
                        if (++consecutive_low >= IR_LOW_DEBOUNCE_SAMPLES) {
                            beam_armed = true;     // pill is currently in beam
                        }
                    } else {
                        consecutive_low = 0;
                        if (++consecutive_high >= IR_HIGH_DEBOUNCE_SAMPLES) {
                            /* Beam went clear after being armed = one
                             * complete pill passage. Count it and disarm
                             * so we're ready for the next pill in this
                             * same servo cycle (multiple pills can drop
                             * from a packed cartridge). */
                            if (beam_armed) {
                                pills_this_cycle++;
                                beam_armed = false;
                            }
                        }
                    }
                } else {
                    fail_reads++;
                }
                vTaskDelay(1);
            }
            /* If the beam is still blocked at end of window, that pill
             * never finished passing — count it anyway so we don't
             * undercount the last pill of a fast burst. The post-home
             * post_clear check below still flags jams separately. */
            if (beam_armed) pills_this_cycle++;
            if ((ir_last & (1 << m_idx)) == 0) post_clear = false;
        } else {
            /* Servo-trust mode (manual / return-pill, no IR).
             * Tight dwells — only enough for the pill to physically
             * drop. Without an IR window to fill we don't need the long
             * 1.5s / 4s / 2s waits. Total per cycle ≈ servo ramp
             * (~1.8s × 2) + dwells (~600ms) = ~4.2s. */
            vTaskDelay(pdMS_TO_TICKS(400));      /* dwell at WORK — pill drops */
            err2 = pca9685_go_home(m_idx);       /* ramp blocking ~1.8s */
            vTaskDelay(pdMS_TO_TICKS(200));      /* settle at HOME */
            pills_this_cycle = 1;                /* trust the servo: 1 pill per cycle */
        }

        if (ir_present) {
            ESP_LOGI(TAG, "  Drop %d/%d IR stats: ok=%d fail=%d ir_min=0x%02X ir_last=0x%02X bit%d_low=%d pills=%d post_clear=%d",
                     i + 1, loops, ok_reads, fail_reads, ir_min, ir_last, m_idx,
                     (ir_min & (1 << m_idx)) ? 0 : 1, pills_this_cycle, post_clear);
        } else {
            ESP_LOGI(TAG, "  Drop %d/%d servo-trust mode (IR off for med%d) — counted as 1 pill",
                     i + 1, loops, m_idx + 1);
        }

        // (Auto-fallback removed — user wants strict IR mode.)
        // For modules with no IR wired, disable via /tech/ir endpoint
        // so the loop starts in servo-trust mode.

        if (err1 != ESP_OK || err2 != ESP_OK) {
            ESP_LOGE(TAG, "Hardware I2C failure during dispense. Aborting task.");
            // Make sure the servo is back at HOME before bailing — otherwise
            // a cup left at WORK position blocks every future dispense until
            // the 90 s busy watchdog clears it.
            (void)pca9685_go_home(m_idx);
            char med_name[64];
            const char *nm = (sh && sh->med[m_idx].name[0])
                               ? sh->med[m_idx].name : telegram_unknown_name();
            snprintf(med_name, sizeof(med_name), "%s", nm);
            char time_str[16] = "--:--";
            ds3231_get_time_str(time_str, sizeof(time_str));
            char msg[384];
            if (telegram_lang_is_th()) {
                snprintf(msg, sizeof(msg),
                         "คืนยาหรือจ่ายยาแบบแมนนวลไม่สำเร็จ\nเวลา: %s\nโมดูล: %d (%s)\nจำนวนที่สั่ง: %s",
                         time_str, m_idx + 1, med_name, requested_str);
            } else {
                snprintf(msg, sizeof(msg),
                         "Manual dispense failed\nTime: %s\nModule: %d (%s)\nRequested: %s",
                         time_str, m_idx + 1, med_name, requested_str);
            }
            send_telegram_photo_or_text(msg);
            ui_manual_disp_status = 3;
            xSemaphoreGive(s_dispense_mutex);
            dispenser_clear_busy();
            vTaskDelete(NULL);
            return;
        }

        // Every servo cycle physically pushes one pill (mechanical
        // guarantee of this dispenser design). Trust the servo: count
        // every cycle as a drop, then subtract the trailing empty
        // cycles that prove the cartridge ran out. IR is used only as
        // the STOP signal, not as the per-pill counter — sub-second
        // PCF8574 backoff windows kept missing actual pill drops which
        // produced wildly low Telegram counts (11 dispensed → 6 reported).
        if (pills_this_cycle > 0) {
            consecutive_empty_cycles = 0;
            actually_dropped += pills_this_cycle;
            int current_count = sh->med[m_idx].count;
            if (current_count > 0) {
                int new_count = current_count - pills_this_cycle;
                if (new_count < 0) new_count = 0;
                netpie_shadow_update_count(m_idx + 1, new_count);
                dispenser_audit_log(m_idx, current_count, new_count, 'M');
                send_low_stock_alert(m_idx, new_count,
                                     "ยาใกล้หมด เหลือ 2 เม็ดสุดท้ายหรือน้อยกว่า",
                                     "Medicine is running low. Two pills or fewer remain.",
                                     false);
            }
        } else {
            // No pill detected this cycle (IR healthy-but-empty OR IR
            // dead). User wants IR to be the sole counter — don't count
            // optimistic/blind drops. Bail after EMPTY_CONFIRM_CYCLES
            // consecutive misses.
            consecutive_empty_cycles++;
            if (!ir_alive) dead_ir_drops++;
            ESP_LOGW(TAG, "Drop %d/%d: no pill seen (ir_alive=%d, empty streak %d/%d)",
                     i + 1, loops, ir_alive ? 1 : 0,
                     consecutive_empty_cycles, EMPTY_CONFIRM_CYCLES);
            if (consecutive_empty_cycles >= EMPTY_CONFIRM_CYCLES &&
                (i + 1) >= EMPTY_MIN_CYCLES_BEFORE_BAIL) {
                ESP_LOGW(TAG, "No pill detected — stopping (cartridge empty or pills not falling)");
                netpie_shadow_update_count(m_idx + 1, 0);
                /* If the user has this med's detail page open on the TFT,
                 * push the IR-confirmed value into the BACK-revert
                 * snapshot too. Without this, pressing Back would revert
                 * count to the pre-dispense value, forcing the user to
                 * manually press บันทึก to keep the system-set 0. */
                extern void ui_setup_meds_resync_backup_count(int, int);
                ui_setup_meds_resync_backup_count(m_idx, 0);
                dispenser_audit_log(m_idx, sh->med[m_idx].count, 0, 'M');
                forced_empty = true;
                send_low_stock_alert(m_idx, 0,
                                     "ไม่พบยาผ่านเซนเซอร์ระหว่างคืนยา/จ่ายยา ระบบตั้งค่ายาคงเหลือเป็น 0 แล้ว กรุณาเติมยา",
                                     "No pill passed the IR sensor during manual dispense/return. Stock was set to 0. Please refill.",
                                     true);
                break;
            }
        }
    }

    ESP_LOGI(TAG, "Manual dispense complete. Dropped: %d pills", actually_dropped);

    // Return-all (eject_all): user expects the cartridge to be empty
    // afterward, so force shadow.count = 0 regardless of how the loop
    // ended (IR-confirmed bail, loop cap, or auto-empty).
    if (eject_all) {
        int curr = sh->med[m_idx].count;
        if (curr != 0) {
            netpie_shadow_update_count(m_idx + 1, 0);
            dispenser_audit_log(m_idx, curr, 0, 'M');
        }
        /* Eject-all: shadow count is now 0. Resync the UI snapshot so
         * a stray Back tap can't revert it. Idempotent — safe even if
         * the user isn't on the detail page. */
        extern void ui_setup_meds_resync_backup_count(int, int);
        ui_setup_meds_resync_backup_count(m_idx, 0);
    }

    // Result audio: pick by *what actually happened*, not by forced_empty.
    //   actually_dropped == 0  → ไม่พบยา (no pill came out at all)
    //   eject_all (return)     → คืนยาเรียบร้อย (return success, even if last
    //                             attempt missed — at least one came out)
    //   manual dispense        → จ่ายยาสำเร็จ
    extern int g_snd_disp_th, g_snd_disp_en, g_snd_return_th, g_snd_return_en, g_snd_nomeds_th, g_snd_nomeds_en;
    bool is_th = telegram_lang_is_th();
    if (actually_dropped == 0) {
        dfplayer_play_track(is_th ? g_snd_nomeds_th : g_snd_nomeds_en);
    } else if (eject_all) {
        dfplayer_play_track(is_th ? g_snd_return_th : g_snd_return_en);
    } else {
        dfplayer_play_track(is_th ? g_snd_disp_th : g_snd_disp_en);
    }

    char med_name[64];
    snprintf(med_name, sizeof(med_name), "%s",
             sh->med[m_idx].name[0] ? sh->med[m_idx].name : telegram_unknown_name());
    char time_str[16] = "--:--";
    ds3231_get_time_str(time_str, sizeof(time_str));
    int remaining_count = netpie_get_shadow()->med[m_idx].count;
    char msg[512];
    // Short summary: module / time / count + photo (sent below).
    // Use IR-confirmed count only — blind/optimistic counts inflated the
    // total when PCF8574 was intermittent (user reported "12 pills" when
    // only 3 actually came out). Truthful undercount is better than
    // inflated count for the user's confirmation message.
    if (telegram_lang_is_th()) {
        if (eject_all) {
            snprintf(msg, sizeof(msg),
                     "🔄 คืนยาทั้งหมด\nโมดูล: %d (%s)\nเวลา: %s\nจ่ายยาคืนทั้งหมดเรียบร้อย",
                     m_idx + 1, med_name, time_str);
        } else {
            snprintf(msg, sizeof(msg),
                     "💊 จ่ายยาแบบแมนนวล\nโมดูล: %d (%s)\nเวลา: %s\nสั่งจ่าย: %d เม็ด",
                     m_idx + 1, med_name, time_str, actually_dropped);
        }
    } else {
        if (eject_all) {
            snprintf(msg, sizeof(msg),
                     "🔄 Return all pills\nModule: %d (%s)\nTime: %s\nAll pills returned successfully",
                     m_idx + 1, med_name, time_str);
        } else {
            snprintf(msg, sizeof(msg),
                     "💊 Manual dispense\nModule: %d (%s)\nTime: %s\nDispensed: %d pills",
                     m_idx + 1, med_name, time_str, actually_dropped);
        }
    }
    /* Release UI BEFORE the Telegram photo upload — JPEG capture +
     * HTTPS POST + optional low-stock follow-up can spend 5-15 s on
     * network I/O, during which the "กำลังจ่ายยา" popup would freeze
     * with no progress visible to the user. The dispense mutex stays
     * held until after the network calls finish so a second dispense
     * can't overlap; the UI just no longer waits on it. */
    ui_manual_disp_status = 2;

    send_telegram_photo_or_text(msg);

    if (!eject_all && actually_dropped < qty) {
        send_low_stock_alert(m_idx, remaining_count,
                             forced_empty
                                 ? "จ่ายยาได้ไม่ครบและระบบตรวจว่าช่องยาน่าจะหมดแล้ว กรุณาเติมยา"
                                 : "จ่ายยาได้ไม่ครบตามจำนวนที่สั่ง กรุณาตรวจสอบและเติมยา",
                             forced_empty
                                 ? "Dispense completed incompletely and the compartment appears empty. Please refill."
                                 : "Dispense completed with fewer pills than requested. Please check and refill.",
                             true);
    } else if (eject_all && forced_empty) {
        send_low_stock_alert(m_idx, remaining_count,
                             "คืนยาหรือจ่ายยาแบบ ALL จนหมดช่องแล้ว กรุณาเติมยา",
                             "ALL dispense/return emptied the compartment. Please refill.",
                             true);
    }

    xSemaphoreGive(s_dispense_mutex);
    dispenser_clear_busy();
    vTaskDelete(NULL);
}

void dispenser_manual_dispense(int med_idx, int qty) {
    if (qty <= 0 || med_idx < 0 || med_idx >= DISPENSER_MED_COUNT) return;
    if (s_emergency_stop) {
        ESP_LOGW(TAG, "Manual dispense blocked: emergency stop active");
        return;
    }
    /* Atomic check-and-claim. Without the critical section, two near-
     * simultaneous callers (e.g. Telegram /dispense + touch UI button)
     * could both pass the overlap check, both call dispenser_mark_busy_if_idle()
     * (only the second fails), and the first's task gets created while
     * the second one's `ui_manual_disp_status` mutation slipped through
     * — leaving the UI in an inconsistent "dispensing" state when no
     * task is actually running. Combined check + busy-mark closes the
     * race window. */
    bool claim_ok = false;
    portENTER_CRITICAL(&s_dispense_state_mux);
    if (ui_manual_disp_status == 0 && !s_waiting_confirm && !s_dispense_approved) {
        /* dispenser_mark_busy_if_idle takes its own critical section
         * which is nested-safe on RISC-V FreeRTOS. */
        claim_ok = dispenser_mark_busy_if_idle();
    }
    portEXIT_CRITICAL(&s_dispense_state_mux);
    if (!claim_ok) return;
    manual_disp_args_t *args = malloc(sizeof(manual_disp_args_t));
    if (args) {
        args->med_idx = med_idx;
        args->qty = qty;
        // Run completely detached from any UI threads
        if (xTaskCreate(manual_dispense_task, "man_disp", MANUAL_DISPENSE_TASK_STACK_SIZE, args, 4, NULL) != pdPASS) {
            free(args);
            dispenser_clear_busy();
            ESP_LOGE(TAG, "Failed to create manual dispense task");
        }
    } else {
        dispenser_clear_busy();
    }
}

/* Public wrapper around send_telegram_photo_or_text() so other modules
 * (UI keyboard, settings) can fire a "with-photo" Telegram notification
 * for events outside the dispenser scheduler proper. */
void dispenser_telegram_photo_msg(const char *msg)
{
    send_telegram_photo_or_text(msg);
}

void dispenser_audit_stock_adjust(int med_idx, int old_count, int new_count)
{
    if (med_idx < 0 || med_idx >= DISPENSER_MED_COUNT) return;
    if (old_count == new_count) return;

    taskENTER_CRITICAL(&s_stock_audit_mux);
    if (!s_stock_audit[med_idx].pending) {
        s_stock_audit[med_idx].start_count = old_count;
    }
    s_stock_audit[med_idx].last_count = new_count;
    s_stock_audit[med_idx].last_change_tick = xTaskGetTickCount();
    s_stock_audit[med_idx].pending = true;
    taskEXIT_CRITICAL(&s_stock_audit_mux);
}
