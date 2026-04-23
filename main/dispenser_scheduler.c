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
static char s_next_dose[32] = "No schedule";

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
        snprintf(s_next_dose, sizeof(s_next_dose), "%s  %s",
                 SLOT_LABELS[best_slot],
                 netpie_get_shadow()->slot_time[best_slot]);
    }
}

static bool s_waiting_confirm = false;
static bool s_empty_stock_warning = false;
static uint32_t s_wait_start_ticks = 0;
static int s_pending_slot_idx = -1;
static bool s_dispense_approved = false;

static bool dispenser_mark_busy_if_idle(void)
{
    bool acquired = false;
    taskENTER_CRITICAL(&s_dispense_state_mux);
    if (!s_dispense_busy) {
        s_dispense_busy = true;
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

    if (has_dispensed && !has_empty && !has_missed) {
        if (telegram_lang_is_th()) {
            snprintf(msg, 768,
                     "✅ ยืนยันการรับยาแล้ว\nเวลา: %s\nมื้อ: %s (%s)\nยาที่จ่ายออกมา: %s",
                     time_str, telegram_slot_label(slot_idx),
                     sh->slot_time[slot_idx], dispensed_meds);
        } else {
            snprintf(msg, 768,
                     "Medication confirmed\nTime: %s\nDose: %s (%s)\nDispensed: %s",
                     time_str, telegram_slot_label(slot_idx),
                     sh->slot_time[slot_idx], dispensed_meds);
        }
        snprintf(detail, 256, "Slot %d (%s) | Out: %s",
                 slot_idx, SLOT_LABELS[slot_idx], dispensed_meds);
        event_name = "Dispensed";
    } else if (has_dispensed) {
        if (telegram_lang_is_th()) {
            snprintf(msg, 768,
                     "⚠️ ยืนยันการรับยาแล้ว\nเวลา: %s\nมื้อ: %s (%s)\nยาที่จ่ายออกมา: %s\nยาที่ควรเติม: %s\nยาที่ต้องตรวจสอบการจ่าย: %s",
                     time_str, telegram_slot_label(slot_idx),
                     sh->slot_time[slot_idx],
                     dispensed_meds,
                     has_empty ? empty_meds : "-",
                     has_missed ? missed_meds : "-");
        } else {
            snprintf(msg, 768,
                     "Medication confirmed with warnings\nTime: %s\nDose: %s (%s)\nDispensed: %s\nRefill: %s\nCheck dispenser: %s",
                     time_str, telegram_slot_label(slot_idx),
                     sh->slot_time[slot_idx],
                     dispensed_meds,
                     has_empty ? empty_meds : "-",
                     has_missed ? missed_meds : "-");
        }
        snprintf(detail, 256, "Slot %d (%s) | Out: %s | Empty: %s | Check: %s",
                 slot_idx, SLOT_LABELS[slot_idx], dispensed_meds,
                 has_empty ? empty_meds : "-", has_missed ? missed_meds : "-");
        event_name = "Partial Dispense";
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

        pca9685_go_work(i);
        uint32_t start_time = esp_log_timestamp();
        while (esp_log_timestamp() - start_time < 1000) {
            uint8_t ir_val = 0xFF;
            if (pcf8574_read(&ir_val) == ESP_OK) {
                if ((ir_val & (1 << i)) == 0) {
                    if (!pill_detected) ESP_LOGI(TAG, "      >> IR sensor %d DETECTED pill (during work)!", i+1);
                    pill_detected = true;
                }
            }
            vTaskDelay(1);
        }

        pca9685_go_home(i);
        start_time = esp_log_timestamp();
        while (esp_log_timestamp() - start_time < 3000) {
            uint8_t ir_val = 0xFF;
            if (pcf8574_read(&ir_val) == ESP_OK) {
                if ((ir_val & (1 << i)) == 0) {
                    if (!pill_detected) ESP_LOGI(TAG, "      >> IR sensor %d DETECTED pill (after home)!", i+1);
                    pill_detected = true;
                }
            }
            vTaskDelay(1);
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
            int new_count = sh->med[i].count - 1;
            if (new_count < 0) new_count = 0;
            netpie_shadow_update_count(i + 1, new_count);
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

        // Check timeout logic
        if (s_waiting_confirm) {
            uint32_t elapsed_sec = (now_ticks - s_wait_start_ticks) * portTICK_PERIOD_MS / 1000;
            if (elapsed_sec > 900) { // 15 mins
                ESP_LOGW(TAG, "User missed medication for slot %d", s_pending_slot_idx);
                g_missed_slots_mask |= (1 << s_pending_slot_idx);
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
                strncpy(s_last_triggered, cur_hhmm, sizeof(s_last_triggered));

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
                    strncpy(s_last_prealert, prealert_key, sizeof(s_last_prealert) - 1);

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
    if (xTaskCreate(dispenser_task, "dispenser", DISPENSER_TASK_STACK_SIZE, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create dispenser scheduler task");
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
        g_missed_slots_mask |= (1 << s_pending_slot_idx);
        
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
    int loops = eject_all ? 100 : qty; // Hard cap 100 loops
    bool forced_empty = false;
    char requested_str[16];
    snprintf(requested_str, sizeof(requested_str), "%s", eject_all ? "ALL" : "");
    if (!eject_all) snprintf(requested_str, sizeof(requested_str), "%d", qty);
    
    const netpie_shadow_t *sh = netpie_get_shadow();

    for (int i = 0; i < loops; i++) {
        ESP_LOGI(TAG, "  💊 Manual Drop %d/%d for med%d", i + 1, loops, m_idx + 1);
        bool pill_detected = false;

        esp_err_t err1 = pca9685_go_work(m_idx);
        uint32_t start = esp_log_timestamp();
        while (esp_log_timestamp() - start < 1000) {
            uint8_t ir = 0xFF;
            if (pcf8574_read(&ir) == ESP_OK && ((ir & (1 << m_idx)) == 0)) pill_detected = true;
            vTaskDelay(1);
        }

        esp_err_t err2 = pca9685_go_home(m_idx);
        start = esp_log_timestamp();
        while (esp_log_timestamp() - start < 3000) {
            uint8_t ir = 0xFF;
            if (pcf8574_read(&ir) == ESP_OK && ((ir & (1 << m_idx)) == 0)) pill_detected = true;
            vTaskDelay(1);
        }

        if (err1 != ESP_OK || err2 != ESP_OK) {
            ESP_LOGE(TAG, "Hardware I2C failure during dispense. Aborting task.");
            char med_name[64];
            snprintf(med_name, sizeof(med_name), "%s",
                     sh->med[m_idx].name[0] ? sh->med[m_idx].name : telegram_unknown_name());
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

        if (pill_detected) {
            actually_dropped++;
            int current_count = sh->med[m_idx].count;
            if (current_count > 0) {
                int new_count = current_count - 1;
                if (new_count < 0) new_count = 0;
                netpie_shadow_update_count(m_idx + 1, new_count);
                send_low_stock_alert(m_idx, new_count,
                                     "ยาใกล้หมด เหลือ 2 เม็ดสุดท้ายหรือน้อยกว่า",
                                     "Medicine is running low. Two pills or fewer remain.",
                                     false);
            }
        } else {
            ESP_LOGW(TAG, "No pill detected during manual dispense/return. Marking stock empty.");
            extern int g_snd_nomeds_th, g_snd_nomeds_en;
            dfplayer_play_track(telegram_lang_is_th() ? g_snd_nomeds_th : g_snd_nomeds_en);
            netpie_shadow_update_count(m_idx + 1, 0);
            forced_empty = true;
            send_low_stock_alert(m_idx, 0,
                                 "ไม่พบยาผ่านเซนเซอร์ระหว่างคืนยา/จ่ายยา ระบบตั้งค่ายาคงเหลือเป็น 0 แล้ว กรุณาเติมยา",
                                 "No pill passed the IR sensor during manual dispense/return. Stock was set to 0. Please refill.",
                                 true);
            break;
        }
    }
    
    ESP_LOGI(TAG, "Manual dispense complete. Dropped: %d pills", actually_dropped);
    // Play result audio via configurable track globals
    extern int g_snd_disp_th, g_snd_disp_en, g_snd_return_th, g_snd_return_en, g_snd_nomeds_th, g_snd_nomeds_en;
    bool is_th = telegram_lang_is_th();
    if (forced_empty || actually_dropped == 0) {
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
    char msg[384];
    if (telegram_lang_is_th()) {
        snprintf(msg, sizeof(msg),
                 "คืนยาหรือจ่ายยาแบบแมนนวลเสร็จแล้ว\nเวลา: %s\nโมดูล: %d (%s)\nจำนวนที่สั่ง: %s\nจ่ายจริง: %d\nคงเหลือ: %d",
                 time_str, m_idx + 1, med_name,
                 requested_str, actually_dropped, remaining_count);
    } else {
        snprintf(msg, sizeof(msg),
                 "Manual dispense completed\nTime: %s\nModule: %d (%s)\nRequested: %s\nDropped: %d\nRemaining: %d",
                 time_str, m_idx + 1, med_name,
                 requested_str, actually_dropped, remaining_count);
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
