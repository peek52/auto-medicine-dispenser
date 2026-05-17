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
#include "ir_input.h"
#include "module_map.h"
#include "i2c_manager.h"
#include "esp_rom_sys.h"
#include "config.h"
#include "telegram_bot.h"
#include "jpeg_encoder.h"
#include "camera_init.h"
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

/* Forward decl — clear-all state vars are defined further down (near
 * clear_all_task) but the scheduler + manual_dispense paths above need
 * to peek at the running flag to refuse work while clear-all owns the
 * dispense mutex. */
static volatile bool s_clear_all_running;

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

// Audit ring: holds scheduled-dispense events for /audit.json AND for
// the Telegram /log command. 256 entries (≈ 2-3 months of typical
// 3-doses/day use) persisted to NVS so it survives reboots — the
// device is the source of truth for its own dose history. NVS blob
// ~3 KB worst case.
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

/* ── Hardware Health state ─────────────────────────────────────────────
 * Set when a critical peripheral failure is detected. UI banner +
 * Telegram alert pick this up. Telegram alert is rate-limited to
 * 1 per hour so a flapping bus doesn't spam the operator. */
/* Forward declaration — actual definition lives further down (near the
 * other telegram helpers, line ~688). Without this, the hw_health_*
 * functions below would emit an implicit-function-declaration error. */
static bool telegram_lang_is_th(void);
static volatile bool s_hw_failed = false;
static char         s_hw_component[32] = {0};
static char         s_hw_detail[96]    = {0};
static portMUX_TYPE s_hw_mux           = portMUX_INITIALIZER_UNLOCKED;
static uint32_t     s_hw_last_alert_ms = 0;  /* esp_log_timestamp scale */
#define HW_ALERT_MIN_GAP_MS  (60UL * 60UL * 1000UL)  /* 1 hour */

void hw_health_set_failure(const char *component, const char *detail)
{
    bool first_or_changed = false;
    portENTER_CRITICAL(&s_hw_mux);
    if (!s_hw_failed) {
        first_or_changed = true;
    } else if (component && strncmp(s_hw_component, component, sizeof(s_hw_component)) != 0) {
        first_or_changed = true;  /* new component took over */
    }
    s_hw_failed = true;
    if (component) {
        strncpy(s_hw_component, component, sizeof(s_hw_component) - 1);
        s_hw_component[sizeof(s_hw_component) - 1] = '\0';
    }
    if (detail) {
        strncpy(s_hw_detail, detail, sizeof(s_hw_detail) - 1);
        s_hw_detail[sizeof(s_hw_detail) - 1] = '\0';
    }
    portEXIT_CRITICAL(&s_hw_mux);

    uint32_t now_ms = esp_log_timestamp();
    bool send_alert = first_or_changed ||
                      (s_hw_last_alert_ms == 0) ||
                      ((now_ms - s_hw_last_alert_ms) >= HW_ALERT_MIN_GAP_MS);
    if (send_alert) {
        s_hw_last_alert_ms = now_ms;
        char msg[256];
        if (telegram_lang_is_th()) {
            snprintf(msg, sizeof(msg),
                     "⚠️ ฮาร์ดแวร์ผิดปกติ\nส่วน: %s\nรายละเอียด: %s\n\n"
                     "กรุณาปิดเครื่องแล้วเปิดใหม่ (Power Cycle) "
                     "เพื่อให้ระบบกลับมาใช้งานได้",
                     component ? component : "Unknown",
                     detail ? detail : "");
        } else {
            snprintf(msg, sizeof(msg),
                     "⚠️ Hardware failure\nComponent: %s\nDetail: %s\n\n"
                     "Please power-cycle the device (turn OFF then ON) "
                     "to recover.",
                     component ? component : "Unknown",
                     detail ? detail : "");
        }
        telegram_send_text(msg);
        ESP_LOGE(TAG, "HW health: FAILURE %s — %s",
                 component ? component : "?", detail ? detail : "");
    }
}

void hw_health_clear_failure(void)
{
    portENTER_CRITICAL(&s_hw_mux);
    bool was_failed = s_hw_failed;
    s_hw_failed = false;
    s_hw_component[0] = '\0';
    s_hw_detail[0] = '\0';
    portEXIT_CRITICAL(&s_hw_mux);
    if (was_failed) {
        ESP_LOGI(TAG, "HW health: cleared");
        if (telegram_lang_is_th()) {
            telegram_send_text("✅ ฮาร์ดแวร์กลับมาใช้งานได้ปกติแล้ว");
        } else {
            telegram_send_text("✅ Hardware recovered");
        }
    }
}

bool hw_health_is_failed(void)
{
    return s_hw_failed;
}

const char *hw_health_get_component(void)
{
    /* Snapshot read — string contents may race with set_failure, but
     * caller only displays it; a torn read on a 31-byte string is
     * cosmetic at worst. */
    return s_hw_failed ? s_hw_component : "";
}

const char *hw_health_get_detail(void)
{
    return s_hw_failed ? s_hw_detail : "";
}

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

/* Forward decl — definition lives further down (after the manual-dispense
 * task). execute_dispense() uses this flag to gate the servo on a real
 * paint of the "DISPENSING..." overlay. */
extern volatile bool g_ui_dispensing_popup_painted;

/* Cross-task signal: after a scheduled dispense, bits set for every
 * med that didn't release a pill (empty cartridge OR IR saw nothing).
 * The display loop pops the lowest set bit, navigates to that module's
 * detail page, and clears the bit — repeated each time the user
 * returns to standby until the mask is empty. So a 3-empty-meds slot
 * walks the user through all three in order. */
volatile uint8_t g_dispense_missed_nav_mask = 0;

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
    return acquired;
}

static void dispenser_clear_busy(void)
{
    taskENTER_CRITICAL(&s_dispense_state_mux);
    s_dispense_busy = false;
    taskEXIT_CRITICAL(&s_dispense_state_mux);
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

void dispenser_format_slots_to_names(uint8_t slots_mask, char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) return;
    buf[0] = '\0';
    if (slots_mask == 0) {
        snprintf(buf, buf_len, "-");
        return;
    }
    bool th = telegram_lang_is_th();
    size_t pos = 0;
    for (int i = 0; i < 7; ++i) {
        if (!((slots_mask >> i) & 0x01)) continue;
        const char *name = th ? TG_SLOT_LABELS_TH[i] : SLOT_LABELS[i];
        int written = snprintf(buf + pos, buf_len - pos,
                               (pos == 0) ? "%s" : ", %s", name);
        if (written < 0 || (size_t)written >= buf_len - pos) break;
        pos += written;
    }
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
                 "⚠️ แจ้งเตือนเติมยา\nเวลา: %s\nโมดูลที่: %d (%s)\nคงเหลือ: %d เม็ด\n%s",
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
            /* Stock-adjust Telegram suppressed 2026-05-15 (user spec
             * "กดบันทึกอย่างเดียวก็พอ"). The audit ledger / count change
             * still flows to the cloud via the normal shadow publish;
             * the summary message fires once when the user hits Save on
             * the meds-detail page. Refill-detection still runs so the
             * low-stock-alert latch resets correctly. */
            (void)send_stock_adjust_audit;   /* keep symbol for any future call */
            reset_low_stock_alert_if_restocked(i, to_count);
        }
    }
}

/* IR level-stability detection — type definition needs to be visible
 * to both execute_dispense() (scheduled dose) and manual_dispense_task()
 * (manual return).  Definitions of the helper functions stay below
 * with the rest of the dispense machinery. */
struct ir_lvl_ctx {
    int  low_streak_ms;
    int  high_streak_ms;
    bool armed;
    int  pills;
    int  total_samples;
    int  blocked_samples;
};
typedef struct ir_lvl_ctx ir_lvl_ctx_t;
static void ir_lvl_init(ir_lvl_ctx_t *c, int ch);
static void ir_count_during_ramp(ir_lvl_ctx_t *c, int ch, int target_angle, uint32_t timeout_ms);

/* ── Execute Dispense Logic (scheduled dose, 2026-05-14 spec) ──
 *
 * Called when the scheduler matches a slot time AND the user has tapped
 * "รับยา" within the grace window (dispenser_confirm_meds set s_dispense_approved).
 *
 * For every med whose slot bit is set in this slot:
 *   - count == 0 → empty_meds (no servo action).
 *   - count > 0  → 1 servo cycle home→work→home with IR pill confirmation.
 *     - IR saw pill   → dispensed_meds, count -= 1.
 *     - IR saw nothing → missed_meds, count forced to 0 (cartridge empty).
 *
 * Reuses the same ir_lvl state-machine as the manual return-pill flow
 * (10 ms continuous LOW = pill confirmed). Summary is sent to Telegram
 * with photo. */
static void execute_dispense(int slot_idx)
{
    if (!dispenser_slot_index_valid(slot_idx)) {
        ESP_LOGE(TAG, "Invalid slot index for execute_dispense: %d", slot_idx);
        dispenser_clear_busy();
        return;
    }
    if (!s_dispense_mutex || xSemaphoreTake(s_dispense_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take dispense mutex for scheduled dispense");
        dispenser_clear_busy();
        return;
    }

    /* Snapshot the shadow into a local copy so the multi-med loop below
     * doesn't race the double-buffered publisher. Each `netpie_shadow_update_count`
     * call inside the loop swaps the pub pointer; after two swaps a
     * pointer-based read would land on the buffer being rewritten,
     * giving torn name/count/slots values mid-dispense. Snapshot is
     * ~700 bytes — fits in dispenser_task's 8 KB stack with margin. */
    netpie_shadow_t snap;
    if (!netpie_shadow_copy(&snap) || !snap.loaded) {
        ESP_LOGE(TAG, "Shadow not loaded — aborting scheduled dispense");
        xSemaphoreGive(s_dispense_mutex);
        dispenser_clear_busy();
        return;
    }
    const netpie_shadow_t *sh = &snap;

    ESP_LOGI(TAG, "Scheduled dispense start: slot=%d (%s)", slot_idx, SLOT_LABELS[slot_idx]);

    /* Wait for the UI to paint the "กำลังจ่ายยา" overlay before the
     * servo spins. ui_confirm_handle_touch paints it synchronously and
     * sets g_ui_dispensing_popup_painted = true before signaling, so
     * this loop usually exits on the first check. The wait is kept as
     * a safety in case the touch handler was bypassed (NETPIE or test
     * paths). Min-visible window guarantees the user actually sees the
     * popup even if the display lagged. */
    {
        const uint32_t POPUP_WAIT_MAX_MS    = 2500;
        const uint32_t POPUP_MIN_VISIBLE_MS = 400;
        uint32_t t0 = esp_log_timestamp();
        while (!g_ui_dispensing_popup_painted &&
               (esp_log_timestamp() - t0) < POPUP_WAIT_MAX_MS) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        while ((esp_log_timestamp() - t0) < POPUP_MIN_VISIBLE_MS) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        ESP_LOGI(TAG, "sched-dispense popup wait done in %lums",
                 (unsigned long)(esp_log_timestamp() - t0));
    }

    enum { MED_LIST_BUF_LEN = 256 };
    char *dispensed_meds = (char *)calloc(1, MED_LIST_BUF_LEN);
    char *empty_meds     = (char *)calloc(1, MED_LIST_BUF_LEN);
    char *missed_meds    = (char *)calloc(1, MED_LIST_BUF_LEN);
    if (!dispensed_meds || !empty_meds || !missed_meds) {
        ESP_LOGE(TAG, "Failed to allocate dispense result buffers");
        free(dispensed_meds);
        free(empty_meds);
        free(missed_meds);
        xSemaphoreGive(s_dispense_mutex);
        dispenser_clear_busy();
        return;
    }

    const uint32_t RAMP_TIMEOUT_MS = 4500;
    uint8_t failed_mask = 0;  /* For UI auto-navigation: bit set per
                                * scheduled med that came up empty or
                                * had IR miss. Display task walks the
                                * mask, one module at a time. */

    for (int i = 0; i < DISPENSER_MED_COUNT; ++i) {
        /* Skip meds that don't belong to this slot. */
        if (!((sh->med[i].slots >> slot_idx) & 0x01)) continue;

        const char *name = sh->med[i].name[0] ? sh->med[i].name : telegram_unknown_name();
        int old_count = sh->med[i].count;

        if (old_count <= 0) {
            ESP_LOGW(TAG, "  med%d (%s) empty — skipping", i + 1, name);
            append_med_name(empty_meds, MED_LIST_BUF_LEN, name);
            /* Audit-skipped per 2026-05-14 spec: only entries where a pill
             * actually came out are kept. Empty-cartridge events go to
             * Telegram only. */
            failed_mask |= (1u << i);
            continue;
        }

        ESP_LOGI(TAG, "  💊 dispensing med%d (%s) count=%d", i + 1, name, old_count);

        /* Logical med i → physical hardware slot via the user-configurable
         * module map (defaults to identity). Use `phys` for every servo
         * and IR call below so a /tech remap reroutes both at once. */
        int phys = module_map_phys_slot(i);

        /* One servo cycle with IR confirmation. */
        ir_lvl_ctx_t ir_ctx;
        ir_lvl_init(&ir_ctx, phys);

        esp_err_t e1 = pca9685_go_work_async(phys);
        if (e1 != ESP_OK) {
            ESP_LOGE(TAG, "  ✗ go_work_async med%d: %s",
                     i + 1, esp_err_to_name(e1));
            append_med_name(missed_meds, MED_LIST_BUF_LEN, name);
            failed_mask |= (1u << i);
            continue;
        }
        ir_count_during_ramp(&ir_ctx, phys, g_servo[phys].work_angle, RAMP_TIMEOUT_MS);

        esp_err_t e2 = pca9685_go_home_async(phys);
        if (e2 != ESP_OK) {
            ESP_LOGE(TAG, "  ✗ go_home_async med%d: %s",
                     i + 1, esp_err_to_name(e2));
            append_med_name(missed_meds, MED_LIST_BUF_LEN, name);
            failed_mask |= (1u << i);
            continue;
        }
        ir_count_during_ramp(&ir_ctx, phys, g_servo[phys].home_angle, RAMP_TIMEOUT_MS);

        bool pill_seen = (ir_ctx.pills > 0);
        ESP_LOGI(TAG, "  → cycle done, IR pills=%d", ir_ctx.pills);

        if (pill_seen) {
            int new_count = old_count - 1;
            if (new_count < 0) new_count = 0;
            netpie_shadow_update_count(i + 1, new_count);
            dispenser_audit_log(i, old_count, new_count, 'S');
            append_med_name(dispensed_meds, MED_LIST_BUF_LEN, name);
            /* Force-publish (in case meds-detail edit-session inhibit
             * is active). */
            char force_payload[64];
            snprintf(force_payload, sizeof(force_payload),
                     "{\"data\":{\"med%d_count\":%d}}", i + 1, new_count);
            netpie_publish_shadow_json(force_payload);
            send_low_stock_alert(i, new_count,
                                 "ยาใกล้หมด เหลือ 2 เม็ดสุดท้ายหรือน้อยกว่า",
                                 "Medicine is running low. Two pills or fewer remain.",
                                 false);
        } else {
            /* IR didn't see a pill — assume cartridge is empty. Force
             * count to 0 + publish. No audit entry: this slot produced
             * no pill, so it doesn't belong in the "ยาที่จ่ายออกมา"
             * history (Telegram still reports it). */
            netpie_shadow_update_count(i + 1, 0);
            char force_payload[64];
            snprintf(force_payload, sizeof(force_payload),
                     "{\"data\":{\"med%d_count\":0}}", i + 1);
            netpie_publish_shadow_json(force_payload);
            extern void ui_setup_meds_resync_backup_count(int, int);
            ui_setup_meds_resync_backup_count(i, 0);
            append_med_name(missed_meds, MED_LIST_BUF_LEN, name);
            failed_mask |= (1u << i);
        }

        vTaskDelay(pdMS_TO_TICKS(200));   /* small gap between meds */
    }

    xSemaphoreGive(s_dispense_mutex);
    dispenser_clear_busy();

    /* Result audio. Two outcomes for the headline clip:
     *   - Nothing came out → "no meds" (nomeds_th/en)
     *   - At least one pill came out → "success" (disp_th/en) — even on
     *     a partial dispense, the success cue plays so the operator
     *     knows the cycle completed; the per-module "empty" voices
     *     below then enumerate which modules failed. */
    extern int g_snd_disp_th, g_snd_disp_en, g_snd_nomeds_th, g_snd_nomeds_en;
    bool any_dispensed = (dispensed_meds[0] != '\0');
    bool is_th = telegram_lang_is_th();
    dfplayer_play_track(any_dispensed
                            ? (is_th ? g_snd_disp_th    : g_snd_disp_en)
                            : (is_th ? g_snd_nomeds_th  : g_snd_nomeds_en));

    /* Per-module empty announcements (user spec 2026-05-15). When any
     * module came up empty or had IR-miss this cycle, play a guided
     * sequence so the operator knows exactly which cartridges need
     * refilling. Track numbering is CONSECUTIVE starting at 0114 —
     * DFPlayer Mini's `play track N` interprets N as "the N-th file
     * physically on the SD card in write order", not as "the file
     * named 0NNN.mp3", so any gap in numbering breaks the mapping
     * (operator confirmed last existing clip is 0113).
     *
     *   TH 114..119 = "โมดูลที่ 1..6 ยาหมด"
     *   EN 120..125 = "Module 1..6 is empty"
     *   TH 126      = lead-in  "มื้อนี้จ่ายยาไม่ครบ โมดูลที่ยาไม่จ่ายคือ"
     *   EN 127      = lead-in  "This dose was incomplete. The empty modules are:"
     *   TH 128      = trailer  "กรุณาเติมยาให้เรียบร้อยค่ะ"
     *   EN 129      = trailer  "Please refill the empty modules"
     *
     * Each clip waits ~2.4 s so the previous one finishes before the
     * next play preempts the dfplayer queue. The dispense task is at
     * the tail of its work here so blocking is acceptable. */
    if (failed_mask) {
        int per_module_base = is_th ? 114 : 120;
        int lead_in_track   = is_th ? 126 : 127;
        int trailer_track   = is_th ? 128 : 129;

        /* Delays generously sized so the longer Thai phrases (~4-5 s)
         * finish before the next play preempts the dfplayer queue —
         * operator reported the lead-in / trailer being cut mid-word
         * 2026-05-15. Trade-off is a longer total announcement (~30 s
         * worst case with all 6 modules), which is acceptable since
         * the dispense task is done at this point. */
        /* Bumped per user report 2026-05-17: the "เสียงจ่ายยาไม่สำเร็จ"
         * headline was getting cut off ("เล่นแปปเดียวเอง เสียงอื่นมา
         * ทับ") because the lead-in queued in to dfplayer too soon.
         * 3000 → 4500 ms gives the longer Thai failure phrase room to
         * finish. */
        const uint32_t WAIT_AFTER_HEADLINE_MS = 4500;   /* "จ่ายยาเรียบร้อย" / "ไม่สำเร็จ" */
        const uint32_t WAIT_AFTER_LEAD_IN_MS  = 5000;   /* long lead-in phrase */
        const uint32_t WAIT_PER_MODULE_MS     = 2800;   /* "โมดูลที่ N ยาหมด" */
        const uint32_t WAIT_AFTER_TRAILER_MS  = 4000;   /* "กรุณาเติมยาให้เรียบร้อยค่ะ" */

        /* Gap after the headline so it isn't cut off. */
        vTaskDelay(pdMS_TO_TICKS(WAIT_AFTER_HEADLINE_MS));

        /* Lead-in: announce that the per-module list is coming. */
        dfplayer_play_track(lead_in_track);
        vTaskDelay(pdMS_TO_TICKS(WAIT_AFTER_LEAD_IN_MS));

        /* Enumerate each failed module in index order. */
        for (int i = 0; i < DISPENSER_MED_COUNT; i++) {
            if (!(failed_mask & (1u << i))) continue;
            dfplayer_play_track(per_module_base + i);
            vTaskDelay(pdMS_TO_TICKS(WAIT_PER_MODULE_MS));
        }

        /* Trailer: gentle ask to refill. Held with its own wait so a
         * subsequent dispense (rare but possible at adjacent slot
         * times) doesn't barge in on the last word. */
        dfplayer_play_track(trailer_track);
        vTaskDelay(pdMS_TO_TICKS(WAIT_AFTER_TRAILER_MS));
    }

    send_dispense_result_summary(slot_idx, dispensed_meds, empty_meds, missed_meds);
    free(dispensed_meds);
    free(empty_meds);
    free(missed_meds);

    /* Reset the paint-ack flag for the next dispense flow. The touch
     * handler will set it true again before signaling. */
    g_ui_dispensing_popup_painted = false;

    /* If any med was empty / missed, signal the display task to walk
     * the user through each failed module one at a time. Use atomic
     * OR so a concurrent bit-clear on the display side can't lose
     * the newly-queued bits (read-modify-write on volatile alone is
     * not atomic on RISC-V). */
    if (failed_mask) {
        __atomic_fetch_or(&g_dispense_missed_nav_mask, failed_mask, __ATOMIC_SEQ_CST);
        ESP_LOGI(TAG, "Queuing UI nav for failed mask=0x%02X", failed_mask);
    }
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
                pca9685_go_home(module_map_phys_slot(i));
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
                    /* No audit row: 'L' missed-dose events removed per
                     * 2026-05-14 spec — history keeps only pills that
                     * actually came out. The miss is still reported to
                     * Telegram (see dispenser_skip_meds). */
                }
                dispenser_skip_meds();
            }
            continue; // Suspend RTC trigger checks while waiting
        }

        // Check dispense logic — e-stop must veto even an already-approved
        // dose. Without this check, an operator who hits E-STOP after the
        // user tapped Confirm but before the scheduler executed would still
        // see the dose dispense. Drop the approval AND clear the pending
        // slot so the dose doesn't fire the moment e-stop is released.
        if (s_dispense_approved && s_emergency_stop) {
            int dropped_slot;
            portENTER_CRITICAL(&s_dispense_state_mux);
            s_dispense_approved = false;
            dropped_slot = s_pending_slot_idx;
            s_pending_slot_idx = -1;
            portEXIT_CRITICAL(&s_dispense_state_mux);
            ESP_LOGW(TAG, "Approved dose for slot %d dropped: emergency stop active",
                     dropped_slot);
        } else if (s_dispense_approved) {
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
                    strlcpy(s_current_date, dt_str, sizeof(s_current_date));
                } else if (strcmp(dt_str, s_current_date) != 0) {
                    // Day changed, reset missed slots mask
                    strlcpy(s_current_date, dt_str, sizeof(s_current_date));
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
            if (!sh || !sh->loaded || !sh->enabled) continue;
            if (s_emergency_stop) continue;  // Skip slot eval entirely while stopped
            if (dispenser_in_quiet_hours(cur_h, cur_m)) {
                // Within user-configured quiet window — auto-skip slot
                // triggers so the elderly user isn't woken up by an
                // alarm at 06:00 if they want quiet until 07:00.
                continue;
            }

            /* Skip every slot evaluation while the boot-clear modal is
             * still up. Otherwise a slot whose time matches during the
             * popup would flip s_waiting_confirm → display_clock yanks
             * the user to PAGE_CONFIRM_MEDS, bypassing the "must press
             * Clear" lock entirely. We also skip while a clear-all is
             * actively running so the scheduler doesn't queue work on
             * top of the worker that has the dispense mutex. The
             * 12-hour refire guard inside the slot loop is intentionally
             * NOT touched here — once boot-clear completes, the slot
             * still gets to fire if its time/grace window is current.
             * Declared local-extern (vs #include "ui_core.h") because
             * ui_core.h drags in Adafruit_GFX C++ deps that don't
             * compile in this C unit. */
            extern bool ui_standby_boot_clear_pending(void);
            if (ui_standby_boot_clear_pending() || s_clear_all_running) {
                /* Fall through to the pre-alert / quiet-hours loops below
                 * is fine — those don't trigger dispense, just warnings. */
                goto skip_slot_eval;
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
                            size_t cur_len = strlen(empty_meds);
                            if (cur_len > 0 && cur_len + 2 < sizeof(empty_meds)) {
                                strncat(empty_meds, ", ", sizeof(empty_meds) - cur_len - 1);
                            }
                            strncat(empty_meds, sh->med[i].name[0] ? sh->med[i].name : "Unknown",
                                    sizeof(empty_meds) - strlen(empty_meds) - 1);
                        }
                    }
                }
                
                if (!has_assigned) continue;

                /* BUG FIX #1: removed `s_last_triggered` global dedup —
                 * it blocked legitimate second-slot fires at the same
                 * minute (e.g. Before-Breakfast 08:00 + Before-Lunch
                 * 08:00 would fire only the first one). Per-slot
                 * 12-h guard above is the correct dedup boundary.
                 *
                 * BUG FIX #5: slot_last_fire stamp moved to
                 * dispenser_confirm_meds() and dispenser_skip_meds().
                 * Stamping at detection blocked the slot for 12h even
                 * if the user later tapped Skip without dispensing.
                 * Scheduler still won't re-evaluate slots while
                 * s_waiting_confirm is true (see line ~1190), so we
                 * don't loop here in the gap before user reacts. */

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
                break; // Stop evaluating other slots
            }
            skip_slot_eval:;

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
    /* BUG FIX #1: s_last_triggered global dedup removed, no longer
     * needs to be reset here. */
    ESP_LOGI(TAG, "Refire guard cleared for slot %d (slot_time changed)", slot_idx);
}
bool dispenser_is_empty_warning(void) { return s_empty_stock_warning; }

bool dispenser_is_busy(void)
{
    bool busy;
    taskENTER_CRITICAL(&s_dispense_state_mux);
    busy = s_dispense_busy || s_dispense_approved;
    taskEXIT_CRITICAL(&s_dispense_state_mux);
    return busy;
}

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
    int  slot_to_stamp = s_pending_slot_idx;
    if (was_waiting) {
        s_dispense_approved = true;
        s_waiting_confirm = false;
    }
    portEXIT_CRITICAL(&s_dispense_state_mux);
    if (was_waiting) {
        ESP_LOGI(TAG, "User CONFIRMED medication drop.");
        /* BUG FIX #5: stamp 12-h refire guard HERE (not at slot
         * detection). Doing it here means a user who taps Skip or
         * the 15-min timeout will get their own short guard via
         * dispenser_skip_meds() — they aren't penalized by a 12-h
         * lockout for a slot they never actually dispensed.
         * Persist to NVS so a reboot just after a confirmed dose
         * doesn't re-trigger the Confirm popup on next boot. */
        if (dispenser_slot_index_valid(slot_to_stamp)) {
            s_slot_last_fire[slot_to_stamp] = time(NULL);
            nvs_handle_t h;
            if (nvs_open("dispenser", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_blob(h, "slot_lastfire",
                             s_slot_last_fire, sizeof(s_slot_last_fire));
                nvs_commit(h);
                nvs_close(h);
            }
        }
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

    ESP_LOGI(TAG, "User SKIPPED medication drop.");
    if (dispenser_slot_index_valid(slot_idx)) {
        g_missed_slots_mask |= (1 << slot_idx);
        /* BUG FIX #5: stamp 12-h refire guard here too (matches
         * dispenser_confirm_meds). Otherwise after a skip the
         * scheduler's grace window (5 min) would re-fire the slot
         * immediately. Persist to NVS so reboot doesn't lose the
         * stamp and re-trigger the popup. */
        s_slot_last_fire[slot_idx] = time(NULL);
        nvs_handle_t h;
        if (nvs_open("dispenser", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, "slot_lastfire",
                         s_slot_last_fire, sizeof(s_slot_last_fire));
            nvs_commit(h);
            nvs_close(h);
        }
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
/* Set to true by ui_setup_meds_detail_render() after the
 * "กำลังดำเนินการ" popup has been fully painted on screen. The
 * dispense task uses this as a synchronization signal: don't start
 * the servo until the user can actually see the popup. */
volatile bool g_ui_dispensing_popup_painted = false;

/* ── Return-pill task (2026-05-13 spec) ──
 *
 *   qty == 100  → คืนยาทั้งหมด: วน cycle ไปเรื่อยๆ จนกว่า IR จะไม่เจอ
 *                  ยาเลยใน 1 cycle (= ตลับว่าง) → หยุด.
 *   qty 1..99   → คืน N เม็ด: วนจน IR นับยาผ่านครบ N หรือ
 *                  มี cycle หนึ่งที่ไม่เจอยาเลย → หยุด.
 *
 *   1 cycle = servo home → work → home (async ramp พร้อม poll IR ตลอด).
 *   IR-edge (blocked→clear) นับเป็น 1 เม็ด ต่อรอบสามารถนับได้หลายเม็ด
 *   ถ้ายาตกพร้อมกัน. ส่งจำนวนที่ IR นับได้จริงไปที่ Telegram. */

/* Level-stability pill counter.
 *
 * Cheap IR comparator modules chatter at high frequency (1-10 kHz),
 * so edge-counting is hopeless.  Instead we sample the pin level at
 * 5 ms and use a state machine that requires the beam to stay LOW
 * for STABLE_LOW_MS continuous milliseconds before counting it as a
 * real pill, then requires STABLE_HIGH_MS continuous milliseconds of
 * HIGH before re-arming.
 *
 *   sample → if (blocked && low_streak ≥ STABLE_LOW_MS && !armed)
 *              → count pill, armed=true
 *            if (!blocked && high_streak ≥ STABLE_HIGH_MS)
 *              → armed=false (ready for next pill)
 *
 * Chatter never sustains a single state long enough → never counts.
 * A real pill blocks the beam for 30-300 ms → counted exactly once. */

#define IR_SAMPLE_MS        5
/* Threshold history:
 *   20 → 15 → 10 ms (latter requested for wider IR detection range).
 * Anything below 10 ms risks counting sub-millisecond comparator
 * chatter as pills — don't go lower without re-validating no-pill
 * tests return 0. */
#define IR_STABLE_LOW_MS    10
#define IR_STABLE_HIGH_MS   10

/* struct definition lives at top of file (forward declared) so
 * execute_dispense can call ir_lvl_init/ir_count_during_ramp too. */

static void ir_lvl_init(ir_lvl_ctx_t *c, int ch)
{
    c->low_streak_ms  = 0;
    c->high_streak_ms = 0;
    c->armed          = ir_blocked(ch);  /* if pill already in beam at start, skip first count */
    c->pills          = 0;
    c->total_samples  = 0;
    c->blocked_samples= 0;
}

static void ir_lvl_feed(ir_lvl_ctx_t *c, bool blocked)
{
    c->total_samples++;
    if (blocked) c->blocked_samples++;

    if (blocked) {
        c->high_streak_ms = 0;
        c->low_streak_ms += IR_SAMPLE_MS;
        if (!c->armed && c->low_streak_ms >= IR_STABLE_LOW_MS) {
            c->armed = true;
            c->pills++;
            ESP_LOGI("ir_lvl", "  PILL detected (low %d ms continuous) total=%d",
                     c->low_streak_ms, c->pills);
        }
    } else {
        c->low_streak_ms = 0;
        c->high_streak_ms += IR_SAMPLE_MS;
        if (c->armed && c->high_streak_ms >= IR_STABLE_HIGH_MS) {
            c->armed = false;   /* re-arm for next pill */
        }
    }
}

static void ir_count_during_ramp(ir_lvl_ctx_t *c, int ch, int target_angle, uint32_t timeout_ms)
{
    uint32_t t0 = esp_log_timestamp();
    while ((esp_log_timestamp() - t0) < timeout_ms) {
        ir_lvl_feed(c, ir_blocked(ch));
        if (g_servo[ch].cur_angle == target_angle) break;
        vTaskDelay(pdMS_TO_TICKS(IR_SAMPLE_MS));
    }
    /* Drain 200 ms after the servo reaches target so a pill still
     * mid-air gets counted. */
    uint32_t drain_until = esp_log_timestamp() + 200;
    while (esp_log_timestamp() < drain_until) {
        ir_lvl_feed(c, ir_blocked(ch));
        vTaskDelay(pdMS_TO_TICKS(IR_SAMPLE_MS));
    }
}

static void manual_dispense_task(void *arg) {
    manual_disp_args_t *args = (manual_disp_args_t *)arg;
    int m_idx = args->med_idx;
    int qty   = args->qty;
    free(args);

    if (!s_dispense_mutex) {
        ESP_LOGE(TAG, "Dispense mutex is not initialized");
        ui_manual_disp_status = 3;
        dispenser_clear_busy();
        vTaskDelete(NULL);
        return;
    }
    if (xSemaphoreTake(s_dispense_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take dispense mutex");
        ui_manual_disp_status = 3;
        dispenser_clear_busy();
        vTaskDelete(NULL);
        return;
    }

    /* status=1 already set by dispenser_manual_dispense() before this
     * task was spawned. Play the audio cue, then poll millis-style
     * (esp_log_timestamp = boot time in ms) for the UI to acknowledge
     * the "กำลังดำเนินการ" popup is on screen. Exits as soon as the
     * flag flips → no fixed delay, max-snappy response. */
    dfplayer_play_track(32);   /* "กำลังจ่ายยา / Processing" */
    {
        const uint32_t POPUP_WAIT_MAX_MS    = 2500;  /* hard timeout for flag */
        const uint32_t POPUP_MIN_VISIBLE_MS = 800;  /* popup MUST stay visible
                                                       * this long before servo
                                                       * starts — display task
                                                       * at prio 3 needs CPU
                                                       * windows to finish the
                                                       * full state-1 popup
                                                       * paint without being
                                                       * preempted */
        uint32_t t0 = esp_log_timestamp();
        uint32_t t_flag = 0;
        while (!g_ui_dispensing_popup_painted &&
               (esp_log_timestamp() - t0) < POPUP_WAIT_MAX_MS) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        t_flag = esp_log_timestamp();
        while ((esp_log_timestamp() - t0) < POPUP_MIN_VISIBLE_MS) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        ESP_LOGI(TAG, "popup wait: flag=%lums total=%lums",
                 (unsigned long)(t_flag - t0),
                 (unsigned long)(esp_log_timestamp() - t0));
    }

    bool eject_all = (qty == 100);
    /* For eject_all, target is the user-configured ceiling so a module
     * loaded above the default 16 still flushes completely. The hard
     * cycle cap stays based on DISPENSER_MAX_PILLS×2 so a truly stuck
     * IR sensor can't loop forever even if the user set max_pills very
     * high. */
    int  target_count = eject_all ? dispenser_max_pills() : qty;
    int  pills_counted = 0;
    int  cycles_run    = 0;
    bool stopped_empty = false;

    ESP_LOGI(TAG, "Return-pill start: med%d %s (target=%d)",
             m_idx + 1, eject_all ? "ALL" : "N-pill", target_count);

    /* Generous per-ramp timeout: 1°/20ms × ≤180° = 3.6 s + slack. */
    const uint32_t RAMP_TIMEOUT_MS = 4500;
    /* Hard cap on cycles — survive a stuck-IR-blocked scenario where
     * count never increments and we'd otherwise loop forever. Sized off
     * the user-config max so the cap scales with what they configured. */
    const int CYCLE_HARD_CAP = dispenser_max_pills() * 2;

    int phys = module_map_phys_slot(m_idx);

    /* Empty-streak tolerance: don't bail on the very first 0-pill cycle —
     * the IR comparator occasionally misses a borderline pill (slightly
     * crooked, or passed the beam too fast). Operator spec 2026-05-15:
     * "ตอนแรกเรากำหนดให้ irไม่เจอยาครั้งเดียวแล้วหยุด ตอนนี้เพื่อเป็นตี
     * อากาสไปเลยเพื่อความชัวร์" — give the cartridge multiple chances
     * before declaring it empty. EMPTY_STREAK_LIMIT=3 means three
     * back-to-back empty cycles are required to confirm. The "got a
     * pill" path resets the streak so a noisy single-miss doesn't
     * shorten the run. */
    const int EMPTY_STREAK_LIMIT = 3;
    int empty_streak = 0;

    while (pills_counted < target_count && cycles_run < CYCLE_HARD_CAP) {
        cycles_run++;

        ir_lvl_ctx_t ir_ctx;
        ir_lvl_init(&ir_ctx, phys);

        /* WORK ramp (home → work). */
        uint32_t t_w0 = esp_log_timestamp();
        ESP_LOGI(TAG, "  → go_work_async med%d (cur=%d → target=%d)",
                 m_idx + 1, g_servo[phys].cur_angle, g_servo[phys].work_angle);
        esp_err_t ew = pca9685_go_work_async(phys);
        if (ew != ESP_OK) {
            ESP_LOGE(TAG, "  go_work_async med%d failed: %s",
                     m_idx + 1, esp_err_to_name(ew));
            break;
        }
        ir_count_during_ramp(&ir_ctx, phys, g_servo[phys].work_angle, RAMP_TIMEOUT_MS);
        ESP_LOGI(TAG, "  ← work done in %lums (pills-so-far=%d)",
                 (unsigned long)(esp_log_timestamp() - t_w0), ir_ctx.pills);

        /* HOME ramp (work → home). */
        uint32_t t_h0 = esp_log_timestamp();
        ESP_LOGI(TAG, "  → go_home_async med%d (cur=%d → target=%d)",
                 m_idx + 1, g_servo[phys].cur_angle, g_servo[phys].home_angle);
        esp_err_t eh = pca9685_go_home_async(phys);
        if (eh != ESP_OK) {
            ESP_LOGE(TAG, "  go_home_async med%d failed: %s",
                     m_idx + 1, esp_err_to_name(eh));
            break;
        }
        ir_count_during_ramp(&ir_ctx, phys, g_servo[phys].home_angle, RAMP_TIMEOUT_MS);
        ESP_LOGI(TAG, "  ← home done in %lums (cur=%d)",
                 (unsigned long)(esp_log_timestamp() - t_h0), g_servo[phys].cur_angle);

        int per_cycle = ir_ctx.pills;
        if (per_cycle > 1) per_cycle = 1;   /* cap: 1 servo cycle = 1 pill */
        pills_counted += per_cycle;
        int blocked_pct = ir_ctx.total_samples
                              ? (100 * ir_ctx.blocked_samples / ir_ctx.total_samples)
                              : 0;
        ESP_LOGI(TAG, "  cycle %d: pills=%d (raw=%d, samples=%d blocked=%d%%) | total=%d",
                 cycles_run, per_cycle, ir_ctx.pills,
                 ir_ctx.total_samples, blocked_pct, pills_counted);

        if (per_cycle == 0) {
            empty_streak++;
            ESP_LOGW(TAG, "  cycle %d: no pill seen (empty streak %d/%d)",
                     cycles_run, empty_streak, EMPTY_STREAK_LIMIT);
            if (empty_streak >= EMPTY_STREAK_LIMIT) {
                stopped_empty = true;
                ESP_LOGW(TAG, "  → %d consecutive empty cycles → cartridge empty, stop",
                         EMPTY_STREAK_LIMIT);
                break;
            }
        } else {
            empty_streak = 0;
        }
    }

    xSemaphoreGive(s_dispense_mutex);

    /* ── Update stock + audit ── */
    const netpie_shadow_t *sh = netpie_get_shadow();
    int new_count = 0;
    int old_count = (sh ? sh->med[m_idx].count : 0);
    if (eject_all || stopped_empty || pills_counted == 0) {
        /* Force count=0 whenever the cartridge is physically empty:
         *   - eject_all: user asked for everything out.
         *   - stopped_empty: a cycle returned 0 pills mid-run → empty.
         *   - pills_counted==0: not a single pill came out → empty.
         * User spec 2026-05-14: "กดคืนยาแล้วยาหมดไม่มียาออก เคลีย
         * เม็ดยาให้เป็น 0 ด้วย". */
        new_count = 0;
    } else {
        new_count = old_count - pills_counted;
        if (new_count < 0) new_count = 0;
    }
    if (sh && new_count != old_count) {
        netpie_shadow_update_count(m_idx + 1, new_count);
        /* No audit entry for return-pill per 2026-05-14 spec —
         * history keeps only scheduled "ยาที่จ่ายออกมา" rows.
         * Telegram still posts a return-complete photo message. */
        extern void ui_setup_meds_resync_backup_count(int, int);
        ui_setup_meds_resync_backup_count(m_idx, new_count);

        /* Force-publish to NETPIE bypassing the publish-inhibit gate.
         * If the user is on the meds-detail page (where +/- edits get
         * batched until Back/Save), netpie_shadow_update_count's
         * publish is dropped. We must still push the dispense-induced
         * count to the cloud — otherwise after a reboot or shadow
         * sync the old (pre-dispense) value comes back. User
         * requirement 2026-05-14: "after return, count must be 0
         * even if user doesn't press save". */
        char force_payload[64];
        snprintf(force_payload, sizeof(force_payload),
                 "{\"data\":{\"med%d_count\":%d}}", m_idx + 1, new_count);
        netpie_publish_shadow_json(force_payload);
    }

    /* ── Audio ── */
    extern int g_snd_disp_th, g_snd_disp_en, g_snd_return_th, g_snd_return_en,
               g_snd_nomeds_th, g_snd_nomeds_en;
    bool is_th = telegram_lang_is_th();
    if (pills_counted == 0) {
        dfplayer_play_track(is_th ? g_snd_nomeds_th : g_snd_nomeds_en);
    } else if (eject_all) {
        dfplayer_play_track(is_th ? g_snd_return_th : g_snd_return_en);
    } else {
        dfplayer_play_track(is_th ? g_snd_disp_th : g_snd_disp_en);
    }

    /* ── Telegram ── */
    char med_name[64];
    snprintf(med_name, sizeof(med_name), "%s",
             (sh && sh->med[m_idx].name[0]) ? sh->med[m_idx].name : telegram_unknown_name());
    char time_str[16] = "--:--";
    ds3231_get_time_str(time_str, sizeof(time_str));
    char msg[512];
    if (telegram_lang_is_th()) {
        if (eject_all) {
            snprintf(msg, sizeof(msg),
                     "🔄 คืนยาทั้งหมด\nโมดูล: %d (%s)\nเวลา: %s\n"
                     "พบยา: %d เม็ด%s",
                     m_idx + 1, med_name, time_str, pills_counted,
                     stopped_empty ? "  (โมดูลว่าง)" : "");
        } else {
            snprintf(msg, sizeof(msg),
                     "💊 คืนยาแบบระบุจำนวน\nโมดูล: %d (%s)\nเวลา: %s\n"
                     "สั่งคืน: %d เม็ด • พบยา: %d เม็ด%s",
                     m_idx + 1, med_name, time_str, qty, pills_counted,
                     stopped_empty ? "  (หยุดเพราะโมดูลว่าง)" : "");
        }
    } else {
        if (eject_all) {
            snprintf(msg, sizeof(msg),
                     "🔄 Return all pills\nModule: %d (%s)\nTime: %s\n"
                     "Found: %d pills%s",
                     m_idx + 1, med_name, time_str, pills_counted,
                     stopped_empty ? "  (module empty)" : "");
        } else {
            snprintf(msg, sizeof(msg),
                     "💊 Return N pills\nModule: %d (%s)\nTime: %s\n"
                     "Requested: %d • Found: %d%s",
                     m_idx + 1, med_name, time_str, qty, pills_counted,
                     stopped_empty ? "  (stopped: empty)" : "");
        }
    }
    ui_manual_disp_status = 2;
    send_telegram_photo_or_text(msg);

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
    /* Refuse if a clear-all is in progress. Without this guard a
     * Telegram /dispense issued during clear-all would mark
     * ui_manual_disp_status=1, spawn manual_dispense_task which then
     * blocks on s_dispense_mutex (portMAX_DELAY) for tens of seconds
     * waiting for clear-all to release. During that wait the UI shows
     * a stuck "กำลังจ่ายยา" popup overlapping the state-8 clear-all
     * paint. Cleanly decline at the entry point instead. */
    if (s_clear_all_running) return;

    /* Status meanings: 0=Idle, 1=Dropping, 2=Success, 3=Fail.
     * 2 and 3 are "done" states held only for the touch UI feedback
     * banner — they get reset to 0 when the user taps to dismiss OR
     * after the meds-detail render loop's idle timeout. The user might
     * never be on that page (e.g. NETPIE remote dispense from standby),
     * so don't gate the next dispense on the user happening to clear
     * the banner. Only refuse while a dispense is actually in flight
     * (status 1) or any scheduled-dose flow is mid-confirm. */
    bool claim_ok = false;
    portENTER_CRITICAL(&s_dispense_state_mux);
    if (ui_manual_disp_status != 1 && !s_waiting_confirm && !s_dispense_approved) {
        /* dispenser_mark_busy_if_idle takes its own critical section
         * which is nested-safe on RISC-V FreeRTOS. */
        claim_ok = dispenser_mark_busy_if_idle();
    }
    portEXIT_CRITICAL(&s_dispense_state_mux);
    if (!claim_ok) return;

    /* Flip the UI popup state RIGHT NOW (before spawning the worker
     * task) so the next render of the meds-detail page paints the
     * "กำลังจ่ายยา" panel without waiting for the task to schedule,
     * take the dispense mutex, and update status. Without this the
     * task can spend 50-200 ms in mutex+log overhead before flipping
     * the flag, and the UI loop sometimes misses the transition.
     *
     * Also clear the paint-acknowledgement flag — the UI will set it
     * back to true after it actually finishes painting the popup,
     * which the dispense task waits for before driving any servo. */
    g_ui_dispensing_popup_painted = false;
    ui_manual_disp_status = 1;

    manual_disp_args_t *args = malloc(sizeof(manual_disp_args_t));
    if (args) {
        args->med_idx = med_idx;
        args->qty = qty;
        // Run completely detached from any UI threads
        /* prio 2 = BELOW display task (prio 3) so the standby/meds-detail
         * render loop can paint popups without being preempted. The
         * actual servo I/O happens via pca9685_ramp_task at prio 5,
         * spawned only after the pre-servo paint sync completes. */
        if (xTaskCreate(manual_dispense_task, "man_disp", MANUAL_DISPENSE_TASK_STACK_SIZE, args, 2, NULL) != pdPASS) {
            free(args);
            ui_manual_disp_status = 0;
            dispenser_clear_busy();
            ESP_LOGE(TAG, "Failed to create manual dispense task");
        }
    } else {
        ui_manual_disp_status = 0;
        dispenser_clear_busy();
    }
}

/* ── Clear-all task (boot-time safety flush) ──
 *
 * Sequentially eject every pill from every module. Used when the user
 * confirms the "all modules report 0 — clear any leftovers" prompt
 * on the standby page. Each module gets the same return-all logic as
 * the manual eject_all path (servo cycles + IR level-stability count)
 * but driven from one task so the operation is atomic from the user's
 * perspective. UI polls the live state via dispenser_clear_all_active()
 * / _current_module() / _pills_total() to update the progress popup. */

static volatile bool s_clear_all_running = false;
static volatile int  s_clear_all_current = -1;  /* 0..5 = working on i, 6 = done */
static volatile int  s_clear_all_pills_total = 0;
/* Running count for the module currently being cleared. clear_one_module
 * writes after each cycle so the UI popup can show "พบยา N เม็ด" live. */
static volatile int  s_clear_all_pills_current = 0;

/* Paint-ack flag: clear_all_task waits for the UI to set this true
 * (signalling the "กำลังล้างยา" popup is on screen) before driving
 * the servo. Same pattern as g_ui_dispensing_popup_painted. */
volatile bool g_ui_clear_all_popup_painted = false;

bool dispenser_clear_all_active(void)         { return s_clear_all_running; }
int  dispenser_clear_all_current_module(void) { return s_clear_all_current; }
int  dispenser_clear_all_pills_total(void)    { return s_clear_all_pills_total; }
int  dispenser_clear_all_pills_current(void)  { return s_clear_all_pills_current; }

static int clear_one_module(int m_idx)
{
    /* Mirror of the manual_dispense_task eject_all loop, simplified
     * for the no-UI clear-all path: no popup status flags here. Per
     * 2026-05-15 spec the caller (clear_all_task) now sends a per-
     * module Telegram message after each return, plus a final total. */
    int pills = 0;
    int cycles_run = 0;
    bool stopped_empty = false;
    /* Cycle cap scales with the configurable max so a module loaded
     * above the default 16 can still be fully flushed. */
    const int CAP = dispenser_max_pills() * 2;
    const uint32_t RAMP_TIMEOUT_MS = 4500;

    s_clear_all_pills_current = 0;   /* live counter for the UI popup */

    int phys = module_map_phys_slot(m_idx);

    /* Same empty-streak tolerance as the manual / scheduled dispense path
     * (see EMPTY_STREAK_LIMIT comment above) — clear-all especially
     * benefits because the last pill in a cartridge often takes 2-3
     * extra cycles to fall through to the IR beam. */
    const int EMPTY_STREAK_LIMIT = 3;
    int empty_streak = 0;

    while (cycles_run < CAP) {
        cycles_run++;
        ir_lvl_ctx_t ctx;
        ir_lvl_init(&ctx, phys);

        if (pca9685_go_work_async(phys) != ESP_OK) break;
        ir_count_during_ramp(&ctx, phys, g_servo[phys].work_angle, RAMP_TIMEOUT_MS);
        if (pca9685_go_home_async(phys) != ESP_OK) break;
        ir_count_during_ramp(&ctx, phys, g_servo[phys].home_angle, RAMP_TIMEOUT_MS);

        int per_cycle = ctx.pills;
        if (per_cycle > 1) per_cycle = 1;
        pills += per_cycle;
        s_clear_all_pills_current = pills;
        ESP_LOGI(TAG, "clear-all M%d cycle %d: pills=%d (streak=%d)",
                 m_idx + 1, cycles_run, per_cycle, empty_streak);
        if (per_cycle == 0) {
            empty_streak++;
            if (empty_streak >= EMPTY_STREAK_LIMIT) {
                stopped_empty = true;
                ESP_LOGW(TAG, "clear-all M%d: %d consecutive empty cycles → done",
                         m_idx + 1, EMPTY_STREAK_LIMIT);
                break;
            }
        } else {
            empty_streak = 0;
        }
    }
    (void)stopped_empty;
    return pills;
}

static void clear_all_task(void *arg)
{
    (void)arg;
    if (!s_dispense_mutex || xSemaphoreTake(s_dispense_mutex, portMAX_DELAY) != pdTRUE) {
        s_clear_all_running = false;
        s_clear_all_current = -1;
        vTaskDelete(NULL);
        return;
    }

    /* Two-phase wait — popup MUST be on screen long enough for the
     * user to register it before any servo motion. */
    {
        const uint32_t POPUP_WAIT_MAX_MS    = 2500;
        const uint32_t POPUP_MIN_VISIBLE_MS = 1500;
        uint32_t t0 = esp_log_timestamp();
        uint32_t t_flag = 0;
        while (!g_ui_clear_all_popup_painted &&
               (esp_log_timestamp() - t0) < POPUP_WAIT_MAX_MS) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        t_flag = esp_log_timestamp();
        while ((esp_log_timestamp() - t0) < POPUP_MIN_VISIBLE_MS) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        ESP_LOGI(TAG, "clear-all popup wait: flag=%lums total=%lums",
                 (unsigned long)(t_flag - t0),
                 (unsigned long)(esp_log_timestamp() - t0));
    }

    s_clear_all_pills_total = 0;
    for (int i = 0; i < DISPENSER_MED_COUNT; ++i) {
        s_clear_all_current = i;
        s_clear_all_pills_current = 0;   /* zero the live tick before the
                                          * first cycle increment lands — avoids
                                          * a brief "Module 2 / Found 7-from-mod-1"
                                          * flash on the popup. */
        int pills = clear_one_module(i);
        s_clear_all_pills_total += pills;
        /* Force shadow count to 0 + force-publish so the cloud reflects
         * the cleared state regardless of any edit-session inhibits. */
        netpie_shadow_update_count(i + 1, 0);
        char force[64];
        snprintf(force, sizeof(force), "{\"data\":{\"med%d_count\":0}}", i + 1);
        netpie_publish_shadow_json(force);
        /* No audit row for clear-all per 2026-05-14 spec — history
         * keeps only scheduled "ยาที่จ่ายออกมา" rows. */

        /* Per-module Telegram notification (user spec 2026-05-15).
         * Sent text-only so the operator can see counts arrive one by
         * one without waiting for the camera snapshot.
         *
         * Copy the name into a local INSIDE the loop. The shadow is
         * double-buffered and a published update during the multi-second
         * clear-all run can flip the buffer that an outer pointer was
         * holding — see audit finding #2. Re-fetching per iteration is
         * cheap (atomic pointer read) and gives a coherent snapshot. */
        char mod_name[32];
        {
            const netpie_shadow_t *sh = netpie_get_shadow();
            const char *src = (sh && sh->med[i].name[0]) ?
                              sh->med[i].name : telegram_unknown_name();
            strncpy(mod_name, src, sizeof(mod_name) - 1);
            mod_name[sizeof(mod_name) - 1] = '\0';
        }
        char mod_msg[160];
        if (telegram_lang_is_th()) {
            snprintf(mod_msg, sizeof(mod_msg),
                     "🧹 ล้างโมดูล %d (%s)\nพบยา %d เม็ด",
                     i + 1, mod_name, pills);
        } else {
            snprintf(mod_msg, sizeof(mod_msg),
                     "🧹 Cleared module %d (%s)\nFound %d pills",
                     i + 1, mod_name, pills);
        }
        telegram_send_text(mod_msg);
    }
    s_clear_all_current = DISPENSER_MED_COUNT;  /* signals "done" */

    /* Suppress the immediate post-clear "confirm to receive medicine"
     * popup. User report 2026-05-15: "เปิดเครื่องมาแล้วกดล้างยาเสร็จ
     * มีการแจ้งเตือนให้กดรับยาด้วย" — if boot-clear happens during an
     * open grace window (e.g. clear at 08:12 with 08:00 dose, grace 15
     * min), the scheduler would still fire the Confirm popup right
     * after clear-all finished.
     *
     * Narrow fix: only stamp slots whose current grace window is OPEN
     * right now (slot_time ≤ now ≤ slot_time + grace). Slots whose grace
     * already expired wouldn't fire anyway; slots in the future stay
     * un-stamped so a user who refills after clearing still gets their
     * later-today doses to dispense normally.
     *
     * The 12-h refire guard (SLOT_REFIRE_GUARD_SEC) handles the rest:
     * once a slot is stamped here, it can't refire until tomorrow.
     * NVS-persisted so a fast reboot right after clear keeps the guard. */
    {
        time_t now_epoch = time(NULL);
        struct tm lt;
        localtime_r(&now_epoch, &lt);
        int now_min = lt.tm_hour * 60 + lt.tm_min;

        const netpie_shadow_t *sh_now = netpie_get_shadow();
        bool stamped_any = false;
        for (int s = 0; s < 7; ++s) {
            int sh_h, sh_m;
            if (!sh_now || !parse_hhmm(sh_now->slot_time[s], &sh_h, &sh_m)) continue;
            int slot_min = sh_h * 60 + sh_m;
            int delta = now_min - slot_min;
            if (delta >= 0 && delta <= SLOT_GRACE_MIN) {
                s_slot_last_fire[s] = now_epoch;
                stamped_any = true;
            }
        }
        if (stamped_any) {
            nvs_handle_t h;
            if (nvs_open("dispenser", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_blob(h, "slot_lastfire",
                             s_slot_last_fire, sizeof(s_slot_last_fire));
                nvs_commit(h);
                nvs_close(h);
            }
        }
    }

    xSemaphoreGive(s_dispense_mutex);

    /* Final summary to Telegram (with photo). */
    char time_str[16] = "--:--";
    ds3231_get_time_str(time_str, sizeof(time_str));
    char msg[256];
    if (telegram_lang_is_th()) {
        snprintf(msg, sizeof(msg),
                 "🧹 ล้างยาทุกโมดูลเรียบร้อย\nเวลา: %s\nรวมทั้งหมด: %d เม็ด",
                 time_str, s_clear_all_pills_total);
    } else {
        snprintf(msg, sizeof(msg),
                 "🧹 All modules cleared\nTime: %s\nTotal: %d pills",
                 time_str, s_clear_all_pills_total);
    }
    send_telegram_photo_or_text(msg);

    /* Leave state at "done" for ~2 s so UI has time to paint the
     * completion message, then reset. */
    vTaskDelay(pdMS_TO_TICKS(2000));
    s_clear_all_current = -1;
    s_clear_all_running = false;
    vTaskDelete(NULL);
}

bool dispenser_clear_all_start(void)
{
    /* Atomic check-and-set so two near-simultaneous callers (e.g. touch
     * Clear-Now + NETPIE "all" command + Telegram trigger) don't both
     * pass the running==false check and spawn duplicate clear_all_tasks.
     * Without this, the second task blocks on s_dispense_mutex then runs
     * the entire 6-module flush again from scratch. */
    bool already_running = false;
    bool dispense_busy_seen = false;
    portENTER_CRITICAL(&s_dispense_state_mux);
    if (s_clear_all_running) {
        already_running = true;
    } else {
        s_clear_all_running = true;
        s_clear_all_current = -1;
    }
    dispense_busy_seen = s_dispense_busy;
    portEXIT_CRITICAL(&s_dispense_state_mux);
    ESP_LOGI(TAG, "dispenser_clear_all_start invoked. already_running=%d dispense_busy=%d e_stop=%d",
             (int)already_running, (int)dispense_busy_seen, (int)s_emergency_stop);
    if (already_running) return false;
    if (s_emergency_stop) {
        /* Don't start clear-all while emergency-stop is active — the
         * task would just spin servos that can't be stopped mid-cycle.
         * Release the running flag we just took so a subsequent call
         * after e-stop clears can succeed. */
        portENTER_CRITICAL(&s_dispense_state_mux);
        s_clear_all_running = false;
        portEXIT_CRITICAL(&s_dispense_state_mux);
        ESP_LOGW(TAG, "Clear-all rejected: emergency stop active");
        return false;
    }

    /* Clear the paint-ack flag BEFORE setting running=true so the
     * UI render loop doesn't latch a stale "painted" from a previous
     * run that wasn't reset (paranoia — task should set it too). */
    g_ui_clear_all_popup_painted = false;
    /* prio 2 — see manual_dispense_task creation for rationale. */
    if (xTaskCreate(clear_all_task, "clear_all", MANUAL_DISPENSE_TASK_STACK_SIZE,
                    NULL, 2, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_dispense_state_mux);
        s_clear_all_running = false;
        portEXIT_CRITICAL(&s_dispense_state_mux);
        ESP_LOGE(TAG, "Failed to spawn clear_all_task");
        return false;
    }
    ESP_LOGI(TAG, "Clear-all started — will iterate %d modules", DISPENSER_MED_COUNT);
    return true;
}

/* Public wrapper around send_telegram_photo_or_text() so other modules
 * (UI keyboard, settings) can fire a "with-photo" Telegram notification
 * for events outside the dispenser scheduler proper. */
void dispenser_telegram_photo_msg(const char *msg)
{
    send_telegram_photo_or_text(msg);
}
