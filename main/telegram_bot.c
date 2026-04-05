#include "telegram_bot.h"
#include "wifi_sta.h"
#include "config.h"
#include "netpie_mqtt.h"
#include "dispenser_scheduler.h"
#include "cloud_secrets.h"
#include "offline_sync.h"
#include "jpeg_encoder.h"
#include "ds3231.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "nvs.h"

static const char *TAG = "TELEGRAM";
static TaskHandle_t s_poll_task_handle = NULL;
static telegram_language_t s_tg_language = TELEGRAM_LANG_EN;

// Async Task payload wrapper
typedef struct {
    char *message;
} tg_task_args_t;

static void telegram_free_text_args(tg_task_args_t *args)
{
    if (!args) return;
    free(args->message);
    free(args);
}

static const char *telegram_pick(const char *en, const char *th)
{
    return (s_tg_language == TELEGRAM_LANG_TH) ? th : en;
}

static void telegram_save_language(void)
{
    nvs_handle_t h;
    if (nvs_open("settings", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "lang_tg", (uint8_t)s_tg_language);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void telegram_load_language(void)
{
    nvs_handle_t h;
    if (nvs_open("settings", NVS_READONLY, &h) == ESP_OK) {
        uint8_t lang = (uint8_t)TELEGRAM_LANG_EN;
        if (nvs_get_u8(h, "lang_tg", &lang) == ESP_OK) {
            s_tg_language = (lang == TELEGRAM_LANG_TH) ? TELEGRAM_LANG_TH : TELEGRAM_LANG_EN;
        }
        nvs_close(h);
    }
}

telegram_language_t telegram_get_language(void)
{
    return s_tg_language;
}

void telegram_set_language(telegram_language_t lang)
{
    s_tg_language = (lang == TELEGRAM_LANG_TH) ? TELEGRAM_LANG_TH : TELEGRAM_LANG_EN;
    telegram_save_language();
}

static char *telegram_build_text_payload(const char *msg)
{
    const char *chat_id = cloud_secrets_get_telegram_chat_id();
    if (!chat_id || !chat_id[0]) return NULL;

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    if (!cJSON_AddStringToObject(root, "chat_id", chat_id) ||
        !cJSON_AddStringToObject(root, "text", msg ? msg : "")) {
        cJSON_Delete(root);
        return NULL;
    }

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return payload;
}

static char *telegram_build_photo_header(const char *boundary, const char *caption, int *out_len)
{
    if (!boundary || !out_len) return NULL;
    const char *chat_id = cloud_secrets_get_telegram_chat_id();
    if (!chat_id || !chat_id[0]) return NULL;

    const char *safe_caption = caption ? caption : "";
    int needed = snprintf(NULL, 0,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
        "%s\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"caption\"\r\n\r\n"
        "%s\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"photo\"; filename=\"photo.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n",
        boundary, chat_id, boundary, safe_caption, boundary);
    if (needed <= 0) return NULL;

    char *header = (char *)malloc((size_t)needed + 1);
    if (!header) return NULL;

    int written = snprintf(header, (size_t)needed + 1,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
        "%s\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"caption\"\r\n\r\n"
        "%s\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"photo\"; filename=\"photo.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n",
        boundary, chat_id, boundary, safe_caption, boundary);
    if (written != needed) {
        free(header);
        return NULL;
    }

    *out_len = written;
    return header;
}

static char *telegram_read_http_response(esp_http_client_handle_t client, int *out_len)
{
    if (!client) return NULL;

    int cap = 2048;
    int total = 0;
    char *buf = (char *)malloc((size_t)cap);
    if (!buf) return NULL;

    while (1) {
        if (cap - total < 512) {
            int new_cap = cap * 2;
            char *new_buf = (char *)realloc(buf, (size_t)new_cap);
            if (!new_buf) {
                free(buf);
                return NULL;
            }
            buf = new_buf;
            cap = new_cap;
        }

        int read_len = esp_http_client_read(client, buf + total, cap - total - 1);
        if (read_len < 0) {
            free(buf);
            return NULL;
        }
        if (read_len == 0) break;
        total += read_len;
    }

    buf[total] = '\0';
    if (out_len) *out_len = total;
    return buf;
}

static bool telegram_is_authorized_chat(const cJSON *msg)
{
    if (!msg) return false;
    const char *expected_chat_id = cloud_secrets_get_telegram_chat_id();
    if (!expected_chat_id || !expected_chat_id[0]) return false;

    cJSON *chat = cJSON_GetObjectItem((cJSON *)msg, "chat");
    if (!cJSON_IsObject(chat)) return false;

    cJSON *id = cJSON_GetObjectItem(chat, "id");
    if (!id || (!cJSON_IsNumber(id) && !cJSON_IsString(id))) return false;

    char chat_id_buf[32];
    if (cJSON_IsString(id) && id->valuestring) {
        snprintf(chat_id_buf, sizeof(chat_id_buf), "%s", id->valuestring);
    } else {
        snprintf(chat_id_buf, sizeof(chat_id_buf), "%.0f", cJSON_GetNumberValue(id));
    }

    return strcmp(chat_id_buf, expected_chat_id) == 0;
}

static bool telegram_extract_command(const char *text, char *out, size_t out_cap)
{
    if (!text || !out || out_cap == 0) return false;

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
    if (*text != '/') return false;

    size_t len = strcspn(text, " \t\r\n");
    if (len == 0) return false;
    if (len >= out_cap) len = out_cap - 1;

    memcpy(out, text, len);
    out[len] = '\0';

    char *at = strchr(out, '@');
    if (at) *at = '\0';
    return out[0] != '\0';
}

static bool telegram_extract_arg1(const char *text, char *out, size_t out_cap)
{
    if (!text || !out || out_cap == 0) return false;

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
    if (*text != '/') return false;

    size_t cmd_len = strcspn(text, " \t\r\n");
    text += cmd_len;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
    if (*text == '\0') return false;

    size_t arg_len = strcspn(text, " \t\r\n");
    if (arg_len == 0) return false;
    if (arg_len >= out_cap) arg_len = out_cap - 1;

    memcpy(out, text, arg_len);
    out[arg_len] = '\0';
    for (size_t i = 0; out[i] != '\0'; ++i) {
        out[i] = (char)tolower((unsigned char)out[i]);
    }
    return true;
}

static void telegram_send_snapshot_reply(const char *caption)
{
    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    if (jpeg_enc_get_frame(&jpg_buf, &jpg_len, 2000) == ESP_OK) {
        uint8_t *copy_buf = (uint8_t *)malloc(jpg_len);
        if (copy_buf) {
            memcpy(copy_buf, jpg_buf, jpg_len);
            telegram_send_photo_with_text(copy_buf, jpg_len, caption);
        } else {
            telegram_send_text(telegram_pick("Snapshot captured, but memory copy failed.",
                                             "ถ่ายภาพได้ แต่คัดลอกหน่วยความจำไม่สำเร็จ"));
        }
        jpeg_enc_release_frame();
    } else {
        telegram_send_text(telegram_pick("Camera snapshot is not available right now.",
                                         "ตอนนี้ยังไม่สามารถถ่ายภาพจากกล้องได้"));
    }
}

static void telegram_send_task(void *pvParameters)
{
    tg_task_args_t *args = (tg_task_args_t *)pvParameters;
    if (!args || !args->message) {
        vTaskDelete(NULL);
        return;
    }

    const char *bot_token = cloud_secrets_get_telegram_token();
    if (!bot_token || !bot_token[0]) {
        telegram_free_text_args(args);
        vTaskDelete(NULL);
        return;
    }

    char url[512];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", bot_token);

    char *post_data = telegram_build_text_payload(args->message);
    if (!post_data) {
        ESP_LOGE(TAG, "Failed to build Telegram JSON payload");
        telegram_free_text_args(args);
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach, // Attach root certificates locally!
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(post_data);
        telegram_free_text_args(args);
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            ESP_LOGI(TAG, "sendMessage OK, status=%d len=%lld",
                     status, esp_http_client_get_content_length(client));
        } else {
            ESP_LOGW(TAG, "sendMessage returned status=%d", status);
            offline_sync_queue_telegram_text(args->message);
        }
    } else {
        ESP_LOGE(TAG, "Error performing HTTP request %s", esp_err_to_name(err));
        offline_sync_queue_telegram_text(args->message);
    }

    esp_http_client_cleanup(client);
    free(post_data);
    telegram_free_text_args(args);
    vTaskDelete(NULL);
}

void telegram_send_text(const char *msg)
{
    if (!msg || !cloud_secrets_has_telegram()) return;

    tg_task_args_t *args = malloc(sizeof(tg_task_args_t));
    if (!args) return;

    args->message = strdup(msg);
    if (!args->message) {
        free(args);
        return;
    }

    // Fire and forget via a background task so it doesn't block the UI or Scheduler threads.
    if (xTaskCreate(telegram_send_task, "tg_send_task", 8192, args, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Telegram send task");
        telegram_free_text_args(args);
    }
}

typedef struct {
    uint8_t *photo_buf;
    size_t photo_len;
    char *caption;
} tg_photo_args_t;

static void telegram_send_photo_task(void *pvParameters) {
    tg_photo_args_t *args = (tg_photo_args_t *)pvParameters;
    const char *bot_token = cloud_secrets_get_telegram_token();
    if (!bot_token || !bot_token[0]) {
        free(args->photo_buf);
        free(args->caption);
        free(args);
        vTaskDelete(NULL);
        return;
    }
    
    char url[512];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendPhoto", bot_token);

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000, 
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(args->photo_buf);
        free(args->caption);
        free(args);
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);

    const char *boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
    
    int header_len = 0;
    char *header = telegram_build_photo_header(boundary, args->caption, &header_len);
    if (!header) {
        ESP_LOGE(TAG, "Failed to build multipart header");
        esp_http_client_cleanup(client);
        free(args->photo_buf);
        free(args->caption);
        free(args);
        vTaskDelete(NULL);
        return;
    }

    char footer[64];
    int footer_len = snprintf(footer, sizeof(footer), "\r\n--%s--\r\n", boundary);

    size_t total_len = (size_t)header_len + args->photo_len + (size_t)footer_len;

    char content_type[128];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);

    esp_err_t err = esp_http_client_open(client, (int)total_len);
    if (err == ESP_OK) {
        esp_http_client_write(client, header, header_len);
        
        // JPEG buffer is huge (~100KB+), esp_http_client_write will drop data if we don't loop it!
        int total_written = 0;
        int retries = 0;
        while (total_written < args->photo_len && retries < 10) {
            int w = esp_http_client_write(client, (const char *)args->photo_buf + total_written, args->photo_len - total_written);
            if (w < 0) {
                ESP_LOGE(TAG, "Write photo binary failed at offset %d", total_written);
                break;
            }
            if (w == 0) {
                 vTaskDelay(pdMS_TO_TICKS(10));
                 retries++;
                 continue;
            }
            total_written += w;
            retries = 0;
        }
        
        if (total_written == (int)args->photo_len) {
            esp_http_client_write(client, footer, footer_len);

            esp_http_client_fetch_headers(client);
            int status = esp_http_client_get_status_code(client);
            if (status == 200) {
                ESP_LOGI(TAG, "sendPhoto OK, status=%d len=%lld",
                         status, esp_http_client_get_content_length(client));
            } else {
                ESP_LOGW(TAG, "sendPhoto returned status=%d", status);
                offline_sync_queue_telegram_text(args->caption);
            }
        } else {
            ESP_LOGE(TAG, "Photo upload incomplete: %d / %u bytes", total_written, (unsigned)args->photo_len);
            offline_sync_queue_telegram_text(args->caption);
        }
    } else {
        ESP_LOGE(TAG, "Failed to open HTTP client for photo: %s", esp_err_to_name(err));
        offline_sync_queue_telegram_text(args->caption);
    }
    
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(header);

    free(args->photo_buf);
    free(args->caption);
    free(args);
    vTaskDelete(NULL);
}

void telegram_send_photo_with_text(uint8_t *photo_buf, size_t photo_len, const char *caption) {
    if (!photo_buf || photo_len == 0 || !cloud_secrets_has_telegram()) {
        free(photo_buf);
        return;
    }
    
    tg_photo_args_t *args = malloc(sizeof(tg_photo_args_t));
    if (!args) {
        free(photo_buf); // Take ownership of buffer to free it
        return;
    }
    
    args->photo_buf = photo_buf;
    args->photo_len = photo_len;
    args->caption = strdup(caption ? caption : "");
    
    if (!args->caption) {
        free(photo_buf);
        free(args);
        return;
    }
    
    if (xTaskCreate(telegram_send_photo_task, "tg_pho_task", 8192, args, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Telegram photo task");
        free(args->photo_buf);
        free(args->caption);
        free(args);
    }
}

void telegram_send_test_message(void)
{
    char time_str[16] = "--:--";
    ds3231_get_time_str(time_str, sizeof(time_str));

    char msg[192];
    snprintf(msg, sizeof(msg), "%s %s",
             telegram_pick("Cloud setup test message sent at",
                           "ข้อความทดสอบจากหน้า Cloud ส่งเมื่อเวลา"),
             time_str);
    telegram_send_text(msg);
}

void telegram_send_test_snapshot(void)
{
    char time_str[16] = "--:--";
    ds3231_get_time_str(time_str, sizeof(time_str));

    char caption[192];
    snprintf(caption, sizeof(caption), "%s %s",
             telegram_pick("Cloud setup test photo captured at",
                           "ภาพทดสอบจากหน้า Cloud ถ่ายเมื่อเวลา"),
             time_str);
    telegram_send_snapshot_reply(caption);
}

static int32_t s_last_update_id = 0;

static void __attribute__((unused)) handle_telegram_command(const char *cmd) {
    if (strcmp(cmd, "/status") == 0 || strcmp(cmd, "/meds") == 0) {
        const netpie_shadow_t *sh = netpie_get_shadow();
        if (!sh->loaded) {
            telegram_send_text("⚠️ แผงยายังไม่ได้ซิงก์ข้อมูลจากคลาวด์ โปรดรอสักครู่...");
            return;
        }
        
        char msg[1024];
        int len = snprintf(msg, sizeof(msg), "🏥 **สถานะตลับยาทั้ง 6 โมดูล**\n\n");
        
        bool has_med = false;
        for (int i = 0; i < DISPENSER_MED_COUNT; i++) {
            if (sh->med[i].name[0] && strlen(sh->med[i].name) > 0) {
                len += snprintf(msg + len, sizeof(msg) - len, "💊 ตลับที่ %d: %s\n📦 เหลือยา: %d เม็ด\n\n", 
                                i + 1, sh->med[i].name, sh->med[i].count);
                has_med = true;
            }
        }
        
        if (!has_med) {
            snprintf(msg + len, sizeof(msg) - len, "❌ ตอนนี้ยังไม่มีการตั้งค่ายาในระบบเลยครับ");
        }
        
        telegram_send_text(msg);
    }
    else if (strcmp(cmd, "/report") == 0) {
        telegram_send_text("กำลังสกัดข้อมูลและรวบรวมสถิติจากประวัติ Google Sheets... กรุณารอสักครู่ครับ 📝📊");
        google_sheets_log("RequestReport", "-", "-");
    }
    else if (strcmp(cmd, "/help") == 0 || strcmp(cmd, "/start") == 0) {
        telegram_send_text("🤖 **Automatic Pill Dispenser Bot**\nพร้อมรับคำสั่งแล้ว!\n\nคำสั่งที่มี:\n/status หรือ /meds - เช็คยอดตลับยาที่เหลือ\n/report - สรุปรายงานการทานยาประจำเดือน\n\n(ระบบแจ้งเตือนการทานยาอัตโนมัติเปิดใช้งานอยู่ ✅)");
    }
}

static void handle_telegram_command_safe(const char *cmd_text)
{
    char cmd[64];
    if (!telegram_extract_command(cmd_text, cmd, sizeof(cmd))) return;

    if (strcmp(cmd, "/status") == 0 || strcmp(cmd, "/meds") == 0) {
        const netpie_shadow_t *sh = netpie_get_shadow();
        if (!sh->loaded) {
            telegram_send_text(telegram_pick("Medication shadow is not synced yet. Please try again shortly.",
                                             "ข้อมูลยายังไม่ซิงก์จากคลาวด์ กรุณาลองใหม่อีกครั้ง"));
            return;
        }

        char msg[1024];
        int len = snprintf(msg, sizeof(msg), "%s",
                           telegram_pick("Medication status for all 6 modules\n\n",
                                         "สถานะยาทั้ง 6 โมดูล\n\n"));

        bool has_med = false;
        for (int i = 0; i < DISPENSER_MED_COUNT && len < (int)sizeof(msg) - 1; i++) {
            if (sh->med[i].name[0] && strlen(sh->med[i].name) > 0) {
                int written = snprintf(msg + len, sizeof(msg) - (size_t)len,
                                       (s_tg_language == TELEGRAM_LANG_TH)
                                           ? "โมดูล %d: %s\nคงเหลือ: %d เม็ด\n\n"
                                           : "Module %d: %s\nRemaining: %d pills\n\n",
                                       i + 1, sh->med[i].name, sh->med[i].count);
                if (written < 0) break;
                if (written >= (int)(sizeof(msg) - (size_t)len)) {
                    len = (int)sizeof(msg) - 1;
                } else {
                    len += written;
                }
                has_med = true;
            }
        }

        if (!has_med && len < (int)sizeof(msg) - 1) {
            (void)snprintf(msg + len, sizeof(msg) - (size_t)len, "%s",
                           telegram_pick("No medicine has been configured yet.",
                                         "ยังไม่มีการตั้งค่ายาในระบบ"));
        }

        telegram_send_text(msg);
    } else if (strcmp(cmd, "/report") == 0) {
        telegram_send_text(telegram_pick("Generating the Google Sheets report. Please wait a moment.",
                                         "กำลังสร้างรายงานจาก Google Sheets กรุณารอสักครู่"));
        google_sheets_log("RequestReport", "-", "-");
    } else if (strcmp(cmd, "/lang") == 0) {
        char arg[16];
        if (!telegram_extract_arg1(cmd_text, arg, sizeof(arg))) {
            telegram_send_text(telegram_pick("Usage: /lang en or /lang th",
                                             "วิธีใช้: /lang en หรือ /lang th"));
            return;
        }

        if (strcmp(arg, "th") == 0 || strcmp(arg, "thai") == 0) {
            telegram_set_language(TELEGRAM_LANG_TH);
            telegram_send_text("เปลี่ยนภาษาข้อความ Telegram เป็นภาษาไทยแล้ว");
        } else if (strcmp(arg, "en") == 0 || strcmp(arg, "eng") == 0 || strcmp(arg, "english") == 0) {
            telegram_set_language(TELEGRAM_LANG_EN);
            telegram_send_text("Telegram message language has been changed to English.");
        } else {
            telegram_send_text(telegram_pick("Unknown language. Use /lang en or /lang th",
                                             "ไม่รู้จักภาษาที่ระบุ ใช้ /lang en หรือ /lang th"));
        }
    } else if (strcmp(cmd, "/photo") == 0 || strcmp(cmd, "/capture") == 0) {
        char time_str[16] = "--:--";
        ds3231_get_time_str(time_str, sizeof(time_str));
        char caption[128];
        snprintf(caption, sizeof(caption), "%s %s",
                 telegram_pick("Live snapshot at", "ภาพถ่ายล่าสุดเวลา"),
                 time_str);
        telegram_send_snapshot_reply(caption);
    } else if (strcmp(cmd, "/help") == 0 || strcmp(cmd, "/start") == 0) {
        telegram_send_text(telegram_pick(
            "Automatic Pill Dispenser Bot is ready.\n\n"
            "Commands:\n"
            "/status or /meds - show remaining pills\n"
            "/report - request monthly report\n"
            "/lang en or /lang th - change Telegram language\n"
            "/photo or /capture - take a live snapshot",
            "บอทเครื่องจ่ายยาพร้อมใช้งาน\n\n"
            "คำสั่ง:\n"
            "/status หรือ /meds - ดูจำนวนยาคงเหลือ\n"
            "/report - ขอรายงานจาก Google Sheets\n"
            "/lang en หรือ /lang th - เปลี่ยนภาษาข้อความ Telegram\n"
            "/photo หรือ /capture - ถ่ายภาพสดจากกล้อง"));
    } else {
        telegram_send_text(telegram_pick("Unknown command. Type /help to see available commands.",
                                         "ไม่รู้จักคำสั่งนี้ พิมพ์ /help เพื่อดูคำสั่งที่ใช้ได้"));
    }
}

static void telegram_poll_task(void *pvParameters) {
    char url[512];
    
    telegram_send_text(telegram_pick(
        "Automatic Pill Dispenser is online.\nType /help or /status to get started.",
        "ระบบ Telegram ของเครื่องจ่ายยาออนไลน์แล้ว\nพิมพ์ /help หรือ /status ได้เลย"));

    while (1) {
        // ── Skip polling entirely when WiFi is offline ────────────────
        // Hammering TCP sockets when not connected exhausts the socket
        // pool and corrupts the WiFi stack, preventing reconnection.
        if (!wifi_sta_connected()) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        const char *bot_token = cloud_secrets_get_telegram_token();
        if (!bot_token || !bot_token[0]) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/getUpdates?offset=%ld&timeout=5", bot_token, (long)(s_last_update_id + 1));

        esp_http_client_config_t config = {
            .url = url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 10000, 
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client) {
            esp_http_client_set_method(client, HTTP_METHOD_GET);
            
            esp_err_t err = esp_http_client_open(client, 0);
            if (err == ESP_OK) {
                esp_http_client_fetch_headers(client);
                int status = esp_http_client_get_status_code(client);
                if (status == 200) {
                    int total_read = 0;
                    char *resp_buf = telegram_read_http_response(client, &total_read);
                    if (resp_buf) {
                        cJSON *root = cJSON_Parse(resp_buf);
                        if (root) {
                            cJSON *ok = cJSON_GetObjectItem(root, "ok");
                            cJSON *result = cJSON_GetObjectItem(root, "result");
                            if (cJSON_IsTrue(ok) && cJSON_IsArray(result)) {
                                int count = cJSON_GetArraySize(result);
                                for (int i = 0; i < count; i++) {
                                    cJSON *item = cJSON_GetArrayItem(result, i);
                                    cJSON *updid = cJSON_GetObjectItem(item, "update_id");
                                    if (!cJSON_IsNumber(updid)) continue;

                                    s_last_update_id = (int32_t)cJSON_GetNumberValue(updid);

                                    cJSON *msg = cJSON_GetObjectItem(item, "message");
                                    if (!cJSON_IsObject(msg)) continue;
                                    if (!telegram_is_authorized_chat(msg)) {
                                        ESP_LOGW(TAG, "Ignoring Telegram command from unauthorized chat");
                                        continue;
                                    }

                                    cJSON *text = cJSON_GetObjectItem(msg, "text");
                                    if (cJSON_IsString(text) && text->valuestring) {
                                        ESP_LOGI(TAG, "TG Rx: %s", text->valuestring);
                                        handle_telegram_command_safe(text->valuestring);
                                    }
                                }
                            } else {
                                ESP_LOGW(TAG, "Telegram response missing ok/result array");
                            }
                            cJSON_Delete(root);
                        } else {
                            ESP_LOGW(TAG, "Failed to parse Telegram response JSON");
                        }
                        free(resp_buf);
                    } else {
                        ESP_LOGE(TAG, "Failed to read Telegram response body");
                    }
                } else {
                    ESP_LOGW(TAG, "getUpdates returned status=%d", status);
                }
            } else {
                ESP_LOGE(TAG, "Failed to open Telegram getUpdates: %s", esp_err_to_name(err));
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
        }
        
        vTaskDelay(pdMS_TO_TICKS(TELEGRAM_CHECK_INTERVAL));
    }
}

void telegram_init(void)
{
    if (s_poll_task_handle) {
        ESP_LOGW(TAG, "Telegram polling task already running");
        return;
    }

    telegram_load_language();
    if (!cloud_secrets_has_telegram()) {
        ESP_LOGW(TAG, "Telegram secrets are missing; bot will stay disabled");
        return;
    }

    ESP_LOGI(TAG, "Starting Telegram Polling Task...");
    if (xTaskCreate(telegram_poll_task, "tg_poll", 8192, NULL, 5, &s_poll_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Telegram polling task");
        s_poll_task_handle = NULL;
    }
}
