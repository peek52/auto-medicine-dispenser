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
#include "cJSON.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Removed g_pre_alert_minutes from here
// Global volume state is now managed dynamically within dfplayer.c directly

static const char *TAG = "dispenser";
static const uint32_t STOCK_AUDIT_IDLE_MS = 2500;

// label แต่ละ slot (ตรงกับ HTML)
static const char *SLOT_LABELS[7] = {
    "Before Breakfast", "After Breakfast", "Before Lunch", "After Lunch", "Before Dinner", "After Dinner", "Bedtime"
};

// ป้องกัน re-trigger ใน 1 นาทีเดียวกัน
static char s_last_triggered[6] = "";   // "HH:MM" ที่ trigger ล่าสุด

// cache next dose string สำหรับ display
static char s_next_dose[32] = "No schedule";

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

static const char *TG_SLOT_LABELS_TH[7] = {
    "ก่อนอาหารเช้า", "หลังอาหารเช้า", "ก่อนอาหารกลางวัน", "หลังอาหารกลางวัน",
    "ก่อนอาหารเย็น", "หลังอาหารเย็น", "ก่อนนอน"
};

/* ── Google Sheets Log Task ── */
typedef struct {
    char event[32];
    char meds[128];
    char detail[128];
} gsheet_args_t;

static void gsheet_post_task(void *pvParam) {
    const char *gs_url = cloud_secrets_get_google_script_url();
    if (!gs_url || gs_url[0] == '\0') {
        vTaskDelete(NULL);
        return;
    }
    gsheet_args_t *args = (gsheet_args_t *)pvParam;
    
    esp_http_client_config_t config = {
        .url = gs_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(args);
        vTaskDelete(NULL);
        return;
    }
    
    cJSON *root = cJSON_CreateObject();
    if (!root ||
        !cJSON_AddStringToObject(root, "event", args->event) ||
        !cJSON_AddStringToObject(root, "meds", args->meds) ||
        !cJSON_AddStringToObject(root, "detail", args->detail)) {
        cJSON_Delete(root);
        esp_http_client_cleanup(client);
        free(args);
        vTaskDelete(NULL);
        return;
    }
    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!post_data) {
        esp_http_client_cleanup(client);
        free(args);
        vTaskDelete(NULL);
        return;
    }
    
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status >= 200 && status < 300) {
            ESP_LOGI(TAG, "GSheet Log Sent! Status = %d", status);
        } else {
            ESP_LOGW(TAG, "GSheet returned status = %d", status);
            offline_sync_queue_google_sheets(args->event, args->meds, args->detail);
        }
    } else {
        ESP_LOGE(TAG, "GSheet HTTP Request failed: %s", esp_err_to_name(err));
        offline_sync_queue_google_sheets(args->event, args->meds, args->detail);
    }
    
    free(post_data);
    esp_http_client_cleanup(client);
    free(args);
    vTaskDelete(NULL);
}

void google_sheets_log(const char *event, const char *meds, const char *detail) {
    if (!cloud_secrets_has_google_script()) return;
    if (!wifi_sta_connected()) {
        offline_sync_queue_google_sheets(event, meds, detail);
        return;
    }
    
    gsheet_args_t *args = malloc(sizeof(gsheet_args_t));
    if (!args) return;
    
    strncpy(args->event, event ? event : "-", sizeof(args->event)-1);
    args->event[sizeof(args->event)-1] = '\0';
    
    strncpy(args->meds, meds ? meds : "-", sizeof(args->meds)-1);
    args->meds[sizeof(args->meds)-1] = '\0';
    
    strncpy(args->detail, detail ? detail : "-", sizeof(args->detail)-1);
    args->detail[sizeof(args->detail)-1] = '\0';
    
    if (xTaskCreate(gsheet_post_task, "gsheet", 4096, args, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Google Sheets task");
        offline_sync_queue_google_sheets(args->event, args->meds, args->detail);
        free(args);
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

static void send_stock_adjust_audit(int med_idx, int from_count, int to_count)
{
    const netpie_shadow_t *sh = netpie_get_shadow();
    if (!sh || med_idx < 0 || med_idx >= DISPENSER_MED_COUNT) return;

    char time_str[16] = "--:--";
    ds3231_get_time_str(time_str, sizeof(time_str));

    char med_name[40];
    snprintf(med_name, sizeof(med_name), "%s",
             sh->med[med_idx].name[0] ? sh->med[med_idx].name : telegram_unknown_name());

    char msg[320];
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

    char detail[96];
    snprintf(detail, sizeof(detail), "Module %d: %d -> %d (%+d)",
             med_idx + 1, from_count, to_count, to_count - from_count);
    google_sheets_log("Stock Adjust", med_name, detail);
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
        }
    }
}

/* ── Execute Dispense Logic (extracted from task) ── */
static void execute_dispense(int slot_idx)
{
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
    for (int i = 0; i < DISPENSER_MED_COUNT; i++) {
        if (!((sh->med[i].slots >> slot_idx) & 1)) continue;
        if (sh->med[i].count <= 0) {
            ESP_LOGW(TAG, "  med%d (%s) empty!", i+1, sh->med[i].name);
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
            char alert_msg[512];
            if (telegram_lang_is_th()) {
                snprintf(alert_msg, sizeof(alert_msg),
                         "⚠️ เกิดข้อผิดพลาดในการจ่ายยา\nโมดูล %d: %s\nเซ็นเซอร์ไม่พบยาตกลงมา\nโปรดตรวจสอบเครื่องจ่ายยา",
                         i + 1, sh->med[i].name[0] ? sh->med[i].name : telegram_unknown_name());
            } else {
                snprintf(alert_msg, sizeof(alert_msg),
                         "Dispense error detected\nModule %d: %s\nThe sensor did not detect a pill drop.\nPlease inspect the dispenser.",
                         i + 1, sh->med[i].name[0] ? sh->med[i].name : telegram_unknown_name());
            }
            telegram_send_text(alert_msg);
            
            // Log Error to Google Sheets
            google_sheets_log("Error - Not Dropped", sh->med[i].name[0] ? sh->med[i].name : "Unknown", "Sensor missed pill drop");
            
        } else {
            ESP_LOGI(TAG, "      ✅ [IR SENSOR] SUCCESS! Pill drop confirmed for med%d", i+1);
        }

        vTaskDelay(pdMS_TO_TICKS(500));

        if (pill_detected) {
            int new_count = sh->med[i].count - 1;
            if (new_count < 0) new_count = 0;
            netpie_shadow_update_count(i + 1, new_count);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    xSemaphoreGive(s_dispense_mutex);
    dispenser_clear_busy();
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
    if (xTaskCreate(dispenser_task, "dispenser", 4096, NULL, 4, NULL) != pdPASS) {
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
        
        char press_time[16] = "--:--:--";
        ds3231_get_time_str(press_time, sizeof(press_time));
        
        char drop_meds[256] = "";
        const netpie_shadow_t *sh = netpie_get_shadow();
        for (int i = 0; i < DISPENSER_MED_COUNT; i++) {
            if (((sh->med[i].slots >> s_pending_slot_idx) & 1) && sh->med[i].count > 0) {
                if (strlen(drop_meds) > 0) strcat(drop_meds, ", ");
                strncat(drop_meds, sh->med[i].name[0] ? sh->med[i].name : "Unknown", sizeof(drop_meds) - strlen(drop_meds) - 1);
            }
        }

        char msg[512];
        if (telegram_lang_is_th()) {
            snprintf(msg, sizeof(msg),
                     "✅ ยืนยันการรับยาเรียบร้อย\nเวลา: %s\nมื้อ: %s (%s)\nยาที่จ่าย: %s",
                     press_time,
                     telegram_slot_label(s_pending_slot_idx),
                     sh->slot_time[s_pending_slot_idx],
                     strlen(drop_meds) > 0 ? drop_meds : "ไม่มี (ยาหมด)");
        } else {
            snprintf(msg, sizeof(msg),
                     "Medication confirmed\nTime: %s\nDose: %s (%s)\nDispensed: %s",
                     press_time,
                     telegram_slot_label(s_pending_slot_idx),
                     sh->slot_time[s_pending_slot_idx],
                     strlen(drop_meds) > 0 ? drop_meds : "None (out of stock)");
        }
        send_telegram_photo_or_text(msg);
        
        // Log Success to Google Sheets
        char detail_str[64];
        snprintf(detail_str, sizeof(detail_str), "Slot %d (%s)", s_pending_slot_idx, SLOT_LABELS[s_pending_slot_idx]);
        google_sheets_log("Dispensed", strlen(drop_meds) > 0 ? drop_meds : "None", detail_str);
    }
}

void dispenser_skip_meds(void) {
    if (s_waiting_confirm) {
        ESP_LOGI(TAG, "User SKIPPED medication drop.");
        
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
        char detail_str[64];
        snprintf(detail_str, sizeof(detail_str), "Slot %d (%s)", s_pending_slot_idx, SLOT_LABELS[s_pending_slot_idx]);
        google_sheets_log("Skipped (Timeout)", "-", detail_str);

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
            char med_name[40];
            snprintf(med_name, sizeof(med_name), "%s",
                     sh->med[m_idx].name[0] ? sh->med[m_idx].name : telegram_unknown_name());
            char time_str[16] = "--:--";
            ds3231_get_time_str(time_str, sizeof(time_str));
            char msg[320];
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
            google_sheets_log("Manual Dispense Fail", med_name, "Hardware I2C failure");
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
                netpie_shadow_update_count(m_idx + 1, current_count - 1);
            }
        } else {
            if (eject_all) {
                ESP_LOGI(TAG, "Eject ALL: No pill detected. Compartment is empty.");
                netpie_shadow_update_count(m_idx + 1, 0);
                break;
            } else {
                ESP_LOGW(TAG, "Missed drop during manual dispense.");
            }
        }
    }
    
    ESP_LOGI(TAG, "Manual dispense complete. Dropped: %d pills", actually_dropped);
    dfplayer_play_track(33); // Track 33: Finished!

    char med_name[40];
    snprintf(med_name, sizeof(med_name), "%s",
             sh->med[m_idx].name[0] ? sh->med[m_idx].name : telegram_unknown_name());
    char time_str[16] = "--:--";
    ds3231_get_time_str(time_str, sizeof(time_str));
    int remaining_count = netpie_get_shadow()->med[m_idx].count;
    char msg[320];
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

    char detail[96];
    snprintf(detail, sizeof(detail), "Module %d dropped %d, left %d",
             m_idx + 1, actually_dropped, remaining_count);
    google_sheets_log("Manual Dispense", med_name, detail);

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
        if (xTaskCreate(manual_dispense_task, "man_disp", 4096, args, 4, NULL) != pdPASS) {
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
