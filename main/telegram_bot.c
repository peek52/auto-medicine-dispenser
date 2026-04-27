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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "nvs.h"

static const char *TAG = "TELEGRAM";
static TaskHandle_t s_poll_task_handle = NULL;
static telegram_language_t s_tg_language = TELEGRAM_LANG_TH;
static SemaphoreHandle_t s_http_mutex = NULL;

// Async Task payload wrapper
typedef struct {
    char *message;
    bool with_keyboard;
} tg_task_args_t;

static void telegram_free_text_args(tg_task_args_t *args)
{
    if (!args) return;
    free(args->message);
    free(args);
}

static bool telegram_http_lock(TickType_t wait_ticks)
{
    if (!s_http_mutex) {
        s_http_mutex = xSemaphoreCreateMutex();
        if (!s_http_mutex) {
            ESP_LOGE(TAG, "Failed to create Telegram HTTP mutex");
            return false;
        }
    }
    return xSemaphoreTake(s_http_mutex, wait_ticks) == pdTRUE;
}

static void telegram_http_unlock(void)
{
    if (s_http_mutex) {
        xSemaphoreGive(s_http_mutex);
    }
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
    s_tg_language = TELEGRAM_LANG_TH;
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

static bool telegram_add_customer_keyboard(cJSON *root)
{
    if (!root) return false;

    cJSON *reply_markup = cJSON_CreateObject();
    cJSON *keyboard = cJSON_CreateArray();
    cJSON *row1 = cJSON_CreateArray();
    cJSON *row2 = cJSON_CreateArray();
    if (!reply_markup || !keyboard || !row1 || !row2) {
        cJSON_Delete(reply_markup);
        cJSON_Delete(keyboard);
        cJSON_Delete(row1);
        cJSON_Delete(row2);
        return false;
    }

    const bool is_th = (s_tg_language == TELEGRAM_LANG_TH);
    cJSON_AddItemToArray(row1, cJSON_CreateString(is_th ? "สถานะยา" : "Medication Status"));
    cJSON_AddItemToArray(row1, cJSON_CreateString(is_th ? "รายงานเดือนนี้" : "Monthly Report"));
    cJSON_AddItemToArray(row2, cJSON_CreateString(is_th ? "ถ่ายภาพล่าสุด" : "Live Photo"));
    cJSON_AddItemToArray(row2, cJSON_CreateString(is_th ? "วิธีใช้" : "Help"));

    cJSON_AddItemToArray(keyboard, row1);
    cJSON_AddItemToArray(keyboard, row2);

    cJSON_AddItemToObject(reply_markup, "keyboard", keyboard);
    cJSON_AddBoolToObject(reply_markup, "resize_keyboard", 1);
    cJSON_AddBoolToObject(reply_markup, "is_persistent", 1);
    cJSON_AddBoolToObject(reply_markup, "one_time_keyboard", 0);

    cJSON_AddItemToObject(root, "reply_markup", reply_markup);
    return true;
}

static char *telegram_build_text_payload(const char *msg, bool with_keyboard)
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

    if (with_keyboard && !telegram_add_customer_keyboard(root)) {
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

static void telegram_trim_ascii_inplace(char *text)
{
    if (!text) return;

    char *start = text;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        ++start;
    }

    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    size_t len = strlen(text);
    while (len > 0) {
        char ch = text[len - 1];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            text[--len] = '\0';
        } else {
            break;
        }
    }
}

static bool telegram_json_id_to_string(const cJSON *obj, const char *field, char *out, size_t out_len)
{
    if (!obj || !field || !out || out_len == 0) return false;

    const cJSON *id = cJSON_GetObjectItem((cJSON *)obj, field);
    if (!id || (!cJSON_IsNumber(id) && !cJSON_IsString(id))) return false;

    if (cJSON_IsString(id) && id->valuestring) {
        snprintf(out, out_len, "%s", id->valuestring);
    } else {
        snprintf(out, out_len, "%.0f", cJSON_GetNumberValue(id));
    }

    telegram_trim_ascii_inplace(out);
    return out[0] != '\0';
}

static bool telegram_is_authorized_chat(const cJSON *msg)
{
    if (!msg) return false;
    const char *expected_chat_id = cloud_secrets_get_telegram_chat_id();
    if (!expected_chat_id || !expected_chat_id[0]) return false;

    char expected_buf[32];
    snprintf(expected_buf, sizeof(expected_buf), "%s", expected_chat_id);
    telegram_trim_ascii_inplace(expected_buf);

    cJSON *chat = cJSON_GetObjectItem((cJSON *)msg, "chat");
    if (cJSON_IsObject(chat)) {
        char chat_id_buf[32];
        if (telegram_json_id_to_string(chat, "id", chat_id_buf, sizeof(chat_id_buf)) &&
            strcmp(chat_id_buf, expected_buf) == 0) {
            return true;
        }
    }

    cJSON *from = cJSON_GetObjectItem((cJSON *)msg, "from");
    if (cJSON_IsObject(from)) {
        char from_id_buf[32];
        if (telegram_json_id_to_string(from, "id", from_id_buf, sizeof(from_id_buf)) &&
            strcmp(from_id_buf, expected_buf) == 0) {
            return true;
        }
    }

    return false;
}

static bool telegram_extract_command(const char *text, char *out, size_t out_cap)
{
    if (!text || !out || out_cap == 0) return false;

    while ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        text += 3;
    }
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

static bool telegram_map_friendly_text_to_command(const char *text, char *out, size_t out_cap)
{
    if (!text || !out || out_cap == 0) return false;

    char normalized[64];
    snprintf(normalized, sizeof(normalized), "%s", text);
    telegram_trim_ascii_inplace(normalized);
    if (!normalized[0]) return false;

    if (strcmp(normalized, "สถานะยา") == 0 || strcmp(normalized, "Medication Status") == 0) {
        snprintf(out, out_cap, "/status");
        return true;
    }
    if (strcmp(normalized, "รายงานเดือนนี้") == 0 || strcmp(normalized, "Monthly Report") == 0) {
        snprintf(out, out_cap, "/report");
        return true;
    }
    if (strcmp(normalized, "ถ่ายภาพล่าสุด") == 0 || strcmp(normalized, "Live Photo") == 0) {
        snprintf(out, out_cap, "/photo");
        return true;
    }
    if (strcmp(normalized, "วิธีใช้") == 0 || strcmp(normalized, "Help") == 0) {
        snprintf(out, out_cap, "/help");
        return true;
    }
    return false;
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

static bool telegram_current_year_month(char *out, size_t out_cap)
{
    if (!out || out_cap < 8) return false;

    char date_buf[24] = {0};
    if (ds3231_get_date_str(date_buf, sizeof(date_buf)) == ESP_OK) {
        int day = 0;
        int month = 0;
        int year = 0;
        if (sscanf(date_buf, "%*3s %d/%d/%d", &day, &month, &year) == 3 &&
            month >= 1 && month <= 12 && year >= 2000) {
            snprintf(out, out_cap, "%04d-%02d", year, month);
            return true;
        }
    }

    time_t now = time(NULL);
    struct tm timeinfo = {0};
#if defined(_WIN32)
    if (localtime_s(&timeinfo, &now) == 0 && timeinfo.tm_year >= 100) {
#else
    if (localtime_r(&now, &timeinfo) && timeinfo.tm_year >= 100) {
#endif
        snprintf(out, out_cap, "%04d-%02d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1);
        return true;
    }

    return false;
}

static bool telegram_normalize_report_month(const char *cmd_text, char *out, size_t out_cap)
{
    if (!out || out_cap < 8) return false;

    char arg[24] = {0};
    if (!telegram_extract_arg1(cmd_text, arg, sizeof(arg))) {
        return telegram_current_year_month(out, out_cap);
    }

    if (strcmp(arg, "now") == 0 || strcmp(arg, "this") == 0 || strcmp(arg, "thismonth") == 0) {
        return telegram_current_year_month(out, out_cap);
    }

    int month = 0;
    int year = 0;
    if ((sscanf(arg, "%d/%d", &month, &year) == 2 ||
         sscanf(arg, "%d-%d", &month, &year) == 2) &&
        month >= 1 && month <= 12 && year >= 2000) {
        snprintf(out, out_cap, "%04d-%02d", year, month);
        return true;
    }

    if ((sscanf(arg, "%d/%d", &year, &month) == 2 ||
         sscanf(arg, "%d-%d", &year, &month) == 2) &&
        year >= 2000 && month >= 1 && month <= 12) {
        snprintf(out, out_cap, "%04d-%02d", year, month);
        return true;
    }

    if (strlen(arg) == 7 && arg[4] == '-' &&
        isdigit((unsigned char)arg[0]) && isdigit((unsigned char)arg[1]) &&
        isdigit((unsigned char)arg[2]) && isdigit((unsigned char)arg[3]) &&
        isdigit((unsigned char)arg[5]) && isdigit((unsigned char)arg[6])) {
        snprintf(out, out_cap, "%s", arg);
        return true;
    }

    return false;
}

static bool telegram_extract_report_text(const char *body, char *out, size_t out_cap)
{
    if (!body || !body[0] || !out || out_cap == 0) return false;

    if (strstr(body, "<!DOCTYPE html") || strstr(body, "<html") ||
        strstr(body, "doGet") || strstr(body, "Function not found") ||
        strstr(body, "ไม่พบฟังก์ชัน")) {
        return false;
    }

    cJSON *root = cJSON_Parse(body);
    if (root) {
        const char *keys[] = { "summary", "report", "message", "text", "data" };
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            cJSON *item = cJSON_GetObjectItem(root, keys[i]);
            if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
                snprintf(out, out_cap, "%s", item->valuestring);
                telegram_trim_ascii_inplace(out);
                cJSON_Delete(root);
                return out[0] != '\0';
            }
        }

        cJSON *error = cJSON_GetObjectItem(root, "error");
        if (cJSON_IsString(error) && error->valuestring && error->valuestring[0]) {
            snprintf(out, out_cap, "%s", error->valuestring);
            telegram_trim_ascii_inplace(out);
            cJSON_Delete(root);
            return out[0] != '\0';
        }

        cJSON_Delete(root);
    }

    snprintf(out, out_cap, "%s", body);
    telegram_trim_ascii_inplace(out);
    return out[0] != '\0';
}

static bool telegram_http_status_ok(int status)
{
    return status >= 200 && status < 400;
}

static bool telegram_http_is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static bool telegram_http_get_text_follow(const char *url, char *out, size_t out_cap, int *out_status, int redirect_depth)
{
    if (!url || !url[0] || !out || out_cap == 0) return false;
    if (redirect_depth > 4) return false;
    if (!telegram_http_lock(pdMS_TO_TICKS(15000))) {
        ESP_LOGW(TAG, "TG report GET lock timeout");
        return false;
    }

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        telegram_http_unlock();
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_GET);
    bool ok = false;
    for (int redirects = redirect_depth; redirects <= 4 && !ok; ++redirects) {
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            break;
        }

        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (out_status) *out_status = status;
        if (telegram_http_is_redirect(status)) {
            ESP_LOGI(TAG, "TG report GET redirect status=%d", status);
            if (esp_http_client_set_redirection(client) != ESP_OK) {
                ESP_LOGW(TAG, "TG report GET redirect setup failed");
                esp_http_client_close(client);
                break;
            }
            esp_http_client_close(client);
            continue;
        } else {
            int body_len = 0;
            char *body = telegram_read_http_response(client, &body_len);
            if (body) {
                ESP_LOGI(TAG, "TG report GET status=%d body=%.180s", status, body);
                ok = telegram_http_status_ok(status) && telegram_extract_report_text(body, out, out_cap);
                free(body);
            } else {
                ESP_LOGW(TAG, "TG report GET status=%d with empty body", status);
            }
            esp_http_client_close(client);
            break;
        }
    }
    esp_http_client_cleanup(client);
    telegram_http_unlock();
    return ok;
}

static bool telegram_http_get_text(const char *url, char *out, size_t out_cap, int *out_status)
{
    return telegram_http_get_text_follow(url, out, out_cap, out_status, 0);
}

static bool telegram_http_post_json_text_follow(const char *url, const char *post_data, char *out, size_t out_cap, int *out_status, int redirect_depth)
{
    if (!url || !url[0] || !post_data || !post_data[0] || !out || out_cap == 0) return false;
    if (redirect_depth > 4) return false;
    if (!telegram_http_lock(pdMS_TO_TICKS(15000))) {
        ESP_LOGW(TAG, "TG report POST lock timeout");
        return false;
    }

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        telegram_http_unlock();
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    bool ok = false;
    esp_err_t err = esp_http_client_open(client, strlen(post_data));
    if (err == ESP_OK) {
        int written = esp_http_client_write(client, post_data, strlen(post_data));
        if (written >= 0) {
            esp_http_client_fetch_headers(client);
            int status = esp_http_client_get_status_code(client);
            if (out_status) *out_status = status;

            if (telegram_http_is_redirect(status)) {
                char *location = NULL;
                if (esp_http_client_get_header(client, "Location", &location) == ESP_OK &&
                    location && location[0]) {
                    esp_http_client_close(client);
                    esp_http_client_cleanup(client);
                    telegram_http_unlock();
                    if (status == 307 || status == 308) {
                        return telegram_http_post_json_text_follow(location, post_data, out, out_cap, out_status, redirect_depth + 1);
                    }
                    return telegram_http_get_text_follow(location, out, out_cap, out_status, redirect_depth + 1);
                }
            } else {
                int body_len = 0;
                char *body = telegram_read_http_response(client, &body_len);
                if (body) {
                    ESP_LOGI(TAG, "TG report POST status=%d body=%.180s", status, body);
                    ok = telegram_http_status_ok(status) && telegram_extract_report_text(body, out, out_cap);
                    free(body);
                } else {
                    ESP_LOGW(TAG, "TG report POST status=%d with empty body", status);
                }
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    telegram_http_unlock();
    return ok;
}

static bool telegram_http_post_json_text(const char *url, const char *post_data, char *out, size_t out_cap, int *out_status)
{
    return telegram_http_post_json_text_follow(url, post_data, out, out_cap, out_status, 0);
}

static void telegram_report_task(void *pvParameters)
{
    tg_task_args_t *args = (tg_task_args_t *)pvParameters;
    char month_key[16] = {0};
    if (args && args->message) {
        snprintf(month_key, sizeof(month_key), "%s", args->message);
    }
    telegram_trim_ascii_inplace(month_key);

    char gs_url[512] = {0};
    snprintf(gs_url, sizeof(gs_url), "%s", cloud_secrets_get_google_script_url());
    telegram_trim_ascii_inplace(gs_url);
    if (!gs_url[0]) {
        telegram_send_text(telegram_pick("Google Sheets is not configured yet.",
                                         "ยังไม่ได้ตั้งค่า Google Sheets"));
        telegram_free_text_args(args);
        vTaskDelete(NULL);
        return;
    }

    char *report_text = (char *)calloc(1, 4096);
    char *url = (char *)calloc(1, 768);
    if (!report_text || !url) {
        free(report_text);
        free(url);
        telegram_send_text(telegram_pick("Unable to allocate memory for the report task.",
                                         "ยังไม่สามารถจองหน่วยความจำสำหรับรายงานได้"));
        telegram_free_text_args(args);
        vTaskDelete(NULL);
        return;
    }
    const char *lang = (s_tg_language == TELEGRAM_LANG_TH) ? "th" : "en";
    char separator = strchr(gs_url, '?') ? '&' : '?';
    snprintf(url, 768, "%s%caction=monthly_report&month=%s&lang=%s&source=telegram",
             gs_url, separator, month_key[0] ? month_key : "current", lang);
    ESP_LOGI(TAG, "TG report start month=%s url=%s",
             month_key[0] ? month_key : "current", url);

    int status = 0;
    bool ok = telegram_http_get_text(url, report_text, 4096, &status);
    ESP_LOGI(TAG, "TG report GET result ok=%d status=%d", ok ? 1 : 0, status);

    if (ok && report_text[0]) {
        if (strlen(report_text) > 3800) {
            report_text[3800] = '\0';
        }
        telegram_trim_ascii_inplace(report_text);
        if (strcmp(report_text, "Report Sent") != 0 &&
            strcmp(report_text, "Success") != 0 &&
            strcmp(report_text, "OK") != 0) {
            telegram_send_text(report_text);
        }
    } else {
        char fallback[512];
        snprintf(fallback, sizeof(fallback), "%s",
                 telegram_pick(
                     "Unable to fetch the monthly Google Sheets report right now.\n"
                     "Expected Apps Script support: action=monthly_report with month=YYYY-MM.\n"
                     "Example: /report 04/2026",
                     "ตอนนี้ยังดึงรายงานรายเดือนจาก Google Sheets ไม่ได้\n"
                     "ฝั่ง Apps Script ต้องรองรับ action=monthly_report และ month=YYYY-MM\n"
                     "ตัวอย่าง: /report 04/2026"));
        telegram_send_text(fallback);
    }

    free(report_text);
    free(url);
    telegram_free_text_args(args);
    vTaskDelete(NULL);
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

// Perform the actual HTTPS POST for a queued text item. Caller transfers
// ownership of *args; this helper frees them before returning.
static void telegram_do_send_text(tg_task_args_t *args)
{
    if (!args || !args->message) {
        telegram_free_text_args(args);
        return;
    }

    const char *bot_token = cloud_secrets_get_telegram_token();
    if (!bot_token || !bot_token[0]) {
        telegram_free_text_args(args);
        return;
    }

    char url[512];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", bot_token);

    char *post_data = telegram_build_text_payload(args->message, args->with_keyboard);
    if (!post_data) {
        ESP_LOGE(TAG, "Failed to build Telegram JSON payload");
        telegram_free_text_args(args);
        return;
    }

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(post_data);
        telegram_free_text_args(args);
        return;
    }

    if (!telegram_http_lock(pdMS_TO_TICKS(15000))) {
        ESP_LOGW(TAG, "sendMessage lock timeout");
        esp_http_client_cleanup(client);
        free(post_data);
        telegram_free_text_args(args);
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
    telegram_http_unlock();
    free(post_data);
    telegram_free_text_args(args);
}

// Single dispatcher task — every telegram_send_text/_with_keyboard caller
// just enqueues a tg_task_args_t* here and returns immediately. Replaces
// the old "spawn 10KB task per call" pattern that could pile up dozens
// of tasks during a busy minute (5 pre-alerts × N modules + bot replies)
// and exhaust heap.
#define TG_TEXT_QUEUE_DEPTH 16
static QueueHandle_t s_tg_text_queue = NULL;
static TaskHandle_t  s_tg_text_worker = NULL;

static void telegram_text_worker(void *arg)
{
    (void)arg;
    for (;;) {
        tg_task_args_t *item = NULL;
        if (xQueueReceive(s_tg_text_queue, &item, portMAX_DELAY) == pdTRUE && item) {
            telegram_do_send_text(item);
        }
    }
}

static bool telegram_text_dispatcher_ready(void)
{
    if (s_tg_text_queue && s_tg_text_worker) return true;
    if (!s_tg_text_queue) {
        s_tg_text_queue = xQueueCreate(TG_TEXT_QUEUE_DEPTH, sizeof(tg_task_args_t *));
        if (!s_tg_text_queue) {
            ESP_LOGE(TAG, "Failed to create Telegram text queue");
            return false;
        }
    }
    if (!s_tg_text_worker) {
        if (xTaskCreate(telegram_text_worker, "tg_text_wrk", 10240,
                        NULL, 5, &s_tg_text_worker) != pdPASS) {
            s_tg_text_worker = NULL;
            ESP_LOGE(TAG, "Failed to create Telegram text worker");
            return false;
        }
    }
    return true;
}

static void telegram_enqueue_text(const char *msg, bool with_keyboard)
{
    if (!msg || !cloud_secrets_has_telegram()) return;
    if (!telegram_text_dispatcher_ready()) return;

    tg_task_args_t *args = malloc(sizeof(tg_task_args_t));
    if (!args) return;
    args->message = strdup(msg);
    if (!args->message) { free(args); return; }
    args->with_keyboard = with_keyboard;

    if (xQueueSend(s_tg_text_queue, &args, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Telegram text queue full — dropping message");
        telegram_free_text_args(args);
    }
}

void telegram_send_text(const char *msg)
{
    telegram_enqueue_text(msg, false);
}

static void telegram_send_text_with_keyboard(const char *msg)
{
    telegram_enqueue_text(msg, true);
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

    if (!telegram_http_lock(pdMS_TO_TICKS(20000))) {
        ESP_LOGW(TAG, "sendPhoto lock timeout");
        esp_http_client_cleanup(client);
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
        telegram_http_unlock();
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
    telegram_http_unlock();
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
    
    if (xTaskCreate(telegram_send_photo_task, "tg_pho_task", 12288, args, 5, NULL) != pdPASS) {
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

static void handle_telegram_command_safe(const char *cmd_text)
{
    char normalized_cmd_text[64];
    if (cmd_text && cmd_text[0] != '/' &&
        telegram_map_friendly_text_to_command(cmd_text, normalized_cmd_text, sizeof(normalized_cmd_text))) {
        cmd_text = normalized_cmd_text;
    }

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
        char month_key[16] = {0};
        if (!telegram_normalize_report_month(cmd_text, month_key, sizeof(month_key))) {
            telegram_send_text(telegram_pick("Usage: /report or /report 04/2026 or /report 2026-04",
                                             "วิธีใช้: /report หรือ /report 04/2026 หรือ /report 2026-04"));
            return;
        }

        char ack[160];
        snprintf(ack, sizeof(ack), "%s %s",
                 telegram_pick("Generating monthly report for", "กำลังสร้างรายงานประจำเดือน"),
                 month_key);
        telegram_send_text(ack);

        tg_task_args_t *args = (tg_task_args_t *)calloc(1, sizeof(tg_task_args_t));
        if (!args) {
            telegram_send_text(telegram_pick("Unable to start report task right now.",
                                             "ยังไม่สามารถเริ่มงานสร้างรายงานได้ในตอนนี้"));
            return;
        }

        args->message = strdup(month_key);
        if (!args->message) {
            telegram_free_text_args(args);
            telegram_send_text(telegram_pick("Unable to allocate memory for the report request.",
                                             "ยังไม่สามารถจองหน่วยความจำสำหรับการขอรายงานได้"));
            return;
        }

        if (xTaskCreate(telegram_report_task, "tg_report", 12288, args, 4, NULL) != pdPASS) {
            telegram_free_text_args(args);
            telegram_send_text(telegram_pick("Unable to start report task right now.",
                                             "ยังไม่สามารถเริ่มงานสร้างรายงานได้ในตอนนี้"));
        }
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
    } else if (strcmp(cmd, "/help") == 0 || strcmp(cmd, "/start") == 0 ||
               strcmp(cmd, "/menu") == 0 || strcmp(cmd, "/commands") == 0) {
        telegram_send_text_with_keyboard(telegram_pick(
            "Automatic Pill Dispenser Bot is ready.\n\n"
            "Use the menu buttons below for the easiest workflow.\n"
            "- Medication Status\n"
            "- Monthly Report\n"
            "- Live Photo\n"
            "- Help\n\n"
            "You can still use /lang en or /lang th anytime.",
            "บอทเครื่องจ่ายยาพร้อมใช้งาน\n\n"
            "กดปุ่มเมนูด้านล่างเพื่อใช้งานได้ง่ายที่สุด\n"
            "- สถานะยา\n"
            "- รายงานเดือนนี้\n"
            "- ถ่ายภาพล่าสุด\n"
            "- Help\n\n"
            "ถ้าต้องการเปลี่ยนภาษา ใช้ /lang en หรือ /lang th\n"
            "ถ้าต้องการดูรายงานของเดือนอื่น ใช้ /report 04/2026"));
    } else {
        telegram_send_text(telegram_pick("Unknown command. Type /help to see available commands.",
                                         "ไม่รู้จักคำสั่งนี้ พิมพ์ /help เพื่อดูคำสั่งที่ใช้ได้"));
    }
}

static void telegram_poll_task(void *pvParameters) {
    char url[512];
    
    telegram_send_text_with_keyboard(telegram_pick(
        "Automatic Pill Dispenser is online.\nTap a menu button below to get started.",
        "ระบบ Telegram ของเครื่องจ่ายยาออนไลน์แล้ว\nกดปุ่มเมนูด้านล่างเพื่อเริ่มใช้งานได้เลย"));

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
            if (!telegram_http_lock(pdMS_TO_TICKS(12000))) {
                ESP_LOGW(TAG, "getUpdates lock timeout");
                esp_http_client_cleanup(client);
                vTaskDelay(pdMS_TO_TICKS(TELEGRAM_CHECK_INTERVAL));
                continue;
            }
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
                                    if (!cJSON_IsObject(msg)) {
                                        msg = cJSON_GetObjectItem(item, "edited_message");
                                    }
                                    if (!cJSON_IsObject(msg)) {
                                        msg = cJSON_GetObjectItem(item, "channel_post");
                                    }
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
            telegram_http_unlock();
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
    if (xTaskCreate(telegram_poll_task, "tg_poll", 12288, NULL, 5, &s_poll_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Telegram polling task");
        s_poll_task_handle = NULL;
    }
}
