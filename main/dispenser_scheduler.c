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

// ป้องกัน re-trigger ใน 1 นาทีเดียวกัน
static char s_last_triggered[6] = "";   // "HH:MM" ที่ trigger ล่าสุด

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

// Per-module IR sensor presence. Default true (assume IR wired). When
// false, dispense skips IR polling and counts every servo cycle as one
// pill — supports installations where only one or two modules actually
// have an IR beam, or where the IR is too unreliable to trust.
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
    uint8_t mask = 0xFF;  // default: all present
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

// Audit ring: holds the last 32 stock-change events for /audit.json. RAM
// only — survives normal task churn but not reboot. Entries newest-first
// when fetched.
#define AUDIT_RING_SIZE 32
static dispenser_audit_entry_t s_audit_ring[AUDIT_RING_SIZE];
static int s_audit_head = 0;     // next write index
static int s_audit_count = 0;    // populated entries (capped at SIZE)
static portMUX_TYPE s_audit_mux = portMUX_INITIALIZER_UNLOCKED;

void dispenser_audit_log(int med_idx, int from_count, int to_count, char source)
{
    if (med_idx < 0 || med_idx >= DISPENSER_MED_COUNT) return;
    if (from_count == to_count) return;
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

    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    if (jpeg_enc_get_frame(&jpg_buf, &jpg_len, 2000) == ESP_OK) {
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

    // Persist the change in the in-memory ring so /audit.json + /tech UI
    // can show it. Source 'V' = sensor sync (this audit path is fired
    // when VL53 sees the cartridge level change after a refill / drain).
    dispenser_audit_log(med_idx, from_count, to_count, 'V');

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
        // and 2 s past home. pcf8574_read takes ~3-5 ms via I2C (the
        // natural pacing); we add a tick-level vTaskDelay every 8
        // reads so the UI/touch task on the same I2C bus gets clear
        // shots and the screen doesn't freeze mid-dispense.
        const uint32_t WORK_MS  = 1500;
        const uint32_t HOME_MS  = 4000;
        const uint32_t POST_MS  = 2000;
        uint32_t loop_start = esp_log_timestamp();
        pca9685_go_work(i);
        bool home_issued = false;
        int read_count = 0;
        while (1) {
            uint32_t elapsed = esp_log_timestamp() - loop_start;
            if (!home_issued && elapsed >= WORK_MS) {
                pca9685_go_home(i);
                home_issued = true;
            }
            if (elapsed >= (WORK_MS + HOME_MS + POST_MS)) break;

            uint8_t ir_val = 0xFF;
            if (pcf8574_read(&ir_val) == ESP_OK) {
                if ((ir_val & (1 << i)) == 0) {
                    if (!pill_detected) {
                        ESP_LOGI(TAG, "      >> IR sensor %d DETECTED pill at %lu ms",
                                 i + 1, (unsigned long)elapsed);
                    }
                    pill_detected = true;
                }
            }
            if ((++read_count % 8) == 0) vTaskDelay(1);
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
            send_low_stock_alert(i, sh->med[i].count,
                                 "มีการยืนยันรับยาแต่ไม่พบยาออกจากช่อง กรุณาตรวจสอบและเติมยา",
                                 "Dose was confirmed but no pill was detected. Please check and refill.",
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
                }
                dispenser_skip_meds();
            }
            continue; // Suspend RTC trigger checks while waiting
        }

        // Check dispense logic
        if (s_dispense_approved) {
            if (dispenser_mark_busy_if_idle()) {
                s_dispense_approved = false;
                execute_dispense(s_pending_slot_idx);
                s_pending_slot_idx = -1; // Clear after dispensing completes
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

                int diff = abs((cur_h*60+cur_m) - (th*60+tm));
                if (diff != 0) continue;
                
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
bool dispenser_is_empty_warning(void) { return s_empty_stock_warning; }

int dispenser_seconds_left(void) {
    if (!s_waiting_confirm) return 0;
    uint32_t elapsed = (xTaskGetTickCount() - s_wait_start_ticks) * portTICK_PERIOD_MS / 1000;
    return elapsed >= 900 ? 0 : (900 - elapsed);
}

int dispenser_waiting_slot(void) { return s_pending_slot_idx; }

void dispenser_confirm_meds(void) {
    if (s_waiting_confirm) {
        s_dispense_approved = true;
        s_waiting_confirm = false;
        ESP_LOGI(TAG, "User CONFIRMED medication drop.");
    }
}

void dispenser_skip_meds(void) {
    if (s_waiting_confirm) {
        ESP_LOGI(TAG, "User SKIPPED medication drop.");
        if (dispenser_slot_index_valid(s_pending_slot_idx)) {
            g_missed_slots_mask |= (1 << s_pending_slot_idx);
        }
        
        char msg[512];
        if (telegram_lang_is_th()) {
            snprintf(msg, sizeof(msg),
                     "❌ ไม่มีผู้มารับยาภายใน 15 นาที\nมื้อ: %s\nระบบจึงไม่จ่ายยาออกมา",
                     telegram_slot_label(s_pending_slot_idx));
        } else {
            snprintf(msg, sizeof(msg),
                     "Medication was not collected within 15 minutes.\nDose: %s\nThe dispenser skipped this round.",
                     telegram_slot_label(s_pending_slot_idx));
        }
        
        send_telegram_photo_or_text(msg);

        // Log Skipped to Google Sheets
        char skipped_meds[256] = "";
        build_slot_med_names(s_pending_slot_idx, skipped_meds, sizeof(skipped_meds), true);
        char detail_str[64];
        snprintf(detail_str, sizeof(detail_str), "Slot %d (%s)", s_pending_slot_idx, SLOT_LABELS[s_pending_slot_idx]);
        google_sheets_log("Skipped (Timeout)", strlen(skipped_meds) > 0 ? skipped_meds : "-", detail_str);

        s_waiting_confirm = false;
        s_pending_slot_idx = -1;
    }
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
    // Servo-trust mode (IR disabled) can't detect "empty" — cap return-all
    // at the cartridge's physical max so we don't cycle forever.
    int loops = eject_all
                  ? (s_ir_present[m_idx] ? 100 : DISPENSER_MAX_PILLS)
                  : qty;
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
    char requested_str[16];
    snprintf(requested_str, sizeof(requested_str), "%s", eject_all ? "ALL" : "");
    if (!eject_all) snprintf(requested_str, sizeof(requested_str), "%d", qty);
    
    const netpie_shadow_t *sh = netpie_get_shadow();
    if (!sh) {
        ESP_LOGE(TAG, "Shadow not loaded — aborting manual dispense");
        ui_manual_disp_status = 3;
        xSemaphoreGive(s_dispense_mutex);
        dispenser_clear_busy();
        vTaskDelete(NULL);
        return;
    }

    // Debounce: pill must be seen LOW for at least this many consecutive
    // polls to count. Reduced 3→1 after users reported missed pills:
    // small/fast pills passed the IR beam in <30 ms (debounce window) so
    // ≥3 consecutive low reads were rare. PCF8574 over I2C is digital
    // and noise-free in practice — a single LOW read is reliable.
    const int IR_LOW_DEBOUNCE_SAMPLES = 1;
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
    bool ir_use = s_ir_present[m_idx];

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
        bool pill_detected = false;
        bool ir_alive = false;        // true if pcf8574_read ever returned ESP_OK
        int  ok_reads = 0;            // diagnostics
        int  fail_reads = 0;
        uint8_t ir_min = 0xFF;        // lowest byte ever observed
        uint8_t ir_last = 0xFF;
        int  consecutive_low = 0;     // debounce counter
        bool ir_present = ir_use;
        bool post_clear = true;

        // Unified continuous polling: servo work → home → 2s post.
        // Light vTaskDelay(1) every 8 reads keeps UI/touch task fed
        // on the shared I2C bus so the screen doesn't freeze.
        const uint32_t POST_HOME_EXTRA_MS = 2000;
        esp_err_t err1 = pca9685_go_work(m_idx);
        uint32_t loop_start = esp_log_timestamp();
        esp_err_t err2 = ESP_OK;
        bool home_issued = false;
        int read_count = 0;
        if (ir_present) {
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
                    if ((ir & (1 << m_idx)) == 0) {
                        if (++consecutive_low >= IR_LOW_DEBOUNCE_SAMPLES) pill_detected = true;
                    } else {
                        consecutive_low = 0;
                    }
                } else {
                    fail_reads++;
                }
                if ((++read_count % 8) == 0) vTaskDelay(1);
            }
            // Post-home clear-check: do we still see a pill jammed in beam?
            if ((ir_last & (1 << m_idx)) == 0) post_clear = false;
        } else {
            // IR disabled / servo-trust mode: just dwell servo.
            vTaskDelay(pdMS_TO_TICKS(IR_WORK_WINDOW_MS));
            err2 = pca9685_go_home(m_idx);
            vTaskDelay(pdMS_TO_TICKS(IR_HOME_WINDOW_MS + POST_HOME_EXTRA_MS));
            pill_detected = true;
        }

        if (ir_present) {
            ESP_LOGI(TAG, "  Drop %d/%d IR stats: ok=%d fail=%d ir_min=0x%02X ir_last=0x%02X bit%d_low=%d detected=%d post_clear=%d",
                     i + 1, loops, ok_reads, fail_reads, ir_min, ir_last, m_idx,
                     (ir_min & (1 << m_idx)) ? 0 : 1, pill_detected, post_clear);
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
        if (pill_detected) {
            consecutive_empty_cycles = 0;
            actually_dropped++;
            int current_count = sh->med[m_idx].count;
            if (current_count > 0) {
                int new_count = current_count - 1;
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
        snprintf(msg, sizeof(msg),
                 "โมดูล: %d (%s)\nเวลา: %s\nจำนวน: %d เม็ด",
                 m_idx + 1, med_name, time_str, actually_dropped);
    } else {
        snprintf(msg, sizeof(msg),
                 "Module: %d (%s)\nTime: %s\nCount: %d pills",
                 m_idx + 1, med_name, time_str, actually_dropped);
    }
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
    ui_manual_disp_status = 2;
    vTaskDelete(NULL);
}

void dispenser_manual_dispense(int med_idx, int qty) {
    if (qty <= 0 || med_idx < 0 || med_idx >= DISPENSER_MED_COUNT) return;
    if (s_emergency_stop) {
        ESP_LOGW(TAG, "Manual dispense blocked: emergency stop active");
        return;
    }
    if (ui_manual_disp_status > 0 || s_waiting_confirm || s_dispense_approved) return; // Prevent overlap with active scheduler/manual flow
    if (!dispenser_mark_busy_if_idle()) return;
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
