#include "telegram_bot.h"
#include "wifi_sta.h"
#include "config.h"
#include "netpie_mqtt.h"
#include "dispenser_scheduler.h"
#include "cloud_secrets.h"
#include "offline_sync.h"
#include "jpeg_encoder.h"
#include "camera_init.h"
#include "ds3231.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
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

// Bound concurrent /photo uploads. Each task owns a 12 KB stack + the JPEG
// buffer (~100 KB), so an unbounded burst of /photo commands could exhaust
// PSRAM/internal RAM before the first upload finishes. Cap to TG_PHOTO_MAX
// in flight; surplus calls drop the buffer and return immediately.
#define TG_PHOTO_MAX_INFLIGHT 2
static volatile int s_photo_inflight = 0;
static portMUX_TYPE s_photo_inflight_mux = portMUX_INITIALIZER_UNLOCKED;

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
    nvs_handle_t h;
    if (nvs_open("settings", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = (uint8_t)TELEGRAM_LANG_TH;
        if (nvs_get_u8(h, "lang_tg", &v) == ESP_OK) {
            s_tg_language = (v == (uint8_t)TELEGRAM_LANG_EN)
                                ? TELEGRAM_LANG_EN
                                : TELEGRAM_LANG_TH;
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
    cJSON_AddItemToArray(row1, cJSON_CreateString(is_th ? "ประวัติการจ่ายยา" : "Dose History"));
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

/* Strip consistency/adherence summary lines from a monthly-report
 * payload so /report shows only the raw date/time log of doses taken.
 * Operates in-place by collapsing dropped lines. Conservative match:
 * drops a line if it contains any of the listed Thai/English keywords
 * OR if it consists mostly of a single "X%" / "X / Y" statistic.
 *
 * Why client-side: the Apps Script that produces this report ships
 * adherence stats by default. We pass detail=log as a hint, but if the
 * script doesn't honor it we still want a clean result. */
static void telegram_strip_consistency_sections(char *text)
{
    if (!text || !*text) return;

    /* Substrings (case-sensitive) — any line containing one of these
     * is dropped. Add new patterns here as needed. */
    static const char *DROP_KEYWORDS[] = {
        "ความสม่ำเสมอ",
        "ความสมำเสมอ",      /* common typo */
        "อัตราการ",
        "อัตรา ",
        "ตรงเวลา",
        "พลาดมื้อ",
        "พลาดยา",
        "สรุปประจำเดือน",
        "สรุปเดือน",
        "เฉลี่ย",
        "Consistency",
        "Adherence",
        "On-time",
        "Overall",
        "Summary",
        "Avg",
        "Average",
        "Missed doses",
    };
    const int N_KW = sizeof(DROP_KEYWORDS) / sizeof(DROP_KEYWORDS[0]);

    char *src = text;
    char *dst = text;
    bool prev_blank = true;   /* collapse leading blank line(s) */
    while (*src) {
        /* Find end of this line. */
        char *eol = src;
        while (*eol && *eol != '\n') ++eol;
        size_t line_len = (size_t)(eol - src);

        bool drop = false;
        for (int k = 0; k < N_KW && !drop; ++k) {
            const char *kw = DROP_KEYWORDS[k];
            size_t kw_len = strlen(kw);
            if (kw_len > line_len) continue;
            /* manual substring search inside [src .. eol) */
            for (size_t i = 0; i + kw_len <= line_len; ++i) {
                if (memcmp(src + i, kw, kw_len) == 0) {
                    drop = true;
                    break;
                }
            }
        }
        /* Drop lines that look like "X%" stats (have a digit followed
         * by a percent sign with no other useful content). */
        if (!drop) {
            bool has_pct = false;
            for (size_t i = 0; i < line_len; ++i) {
                if (src[i] == '%' && i > 0 &&
                    (src[i-1] >= '0' && src[i-1] <= '9')) {
                    has_pct = true;
                    break;
                }
            }
            if (has_pct) drop = true;
        }

        bool is_blank = (line_len == 0);
        if (!drop) {
            /* Avoid stacking consecutive blank lines after dropping
             * something — looks nicer in Telegram. */
            if (!(is_blank && prev_blank)) {
                memmove(dst, src, line_len);
                dst += line_len;
                if (*eol == '\n') { *dst++ = '\n'; }
            }
            prev_blank = is_blank;
        } else {
            prev_blank = true;
        }

        src = (*eol == '\n') ? eol + 1 : eol;
    }
    *dst = '\0';
    /* Tidy up trailing blank lines. */
    while (dst > text && (dst[-1] == '\n' || dst[-1] == ' ' || dst[-1] == '\r' || dst[-1] == '\t')) {
        *--dst = '\0';
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
    if (strcmp(normalized, "ประวัติการจ่ายยา") == 0 || strcmp(normalized, "Dose History") == 0) {
        snprintf(out, out_cap, "/log");
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
    // Reject (instead of silently truncate) over-long args so the
    // caller can tell the user the command was malformed instead of
    // running a misparsed report month / med name / etc.
    if (arg_len >= out_cap) {
        ESP_LOGW(TAG, "extract_arg1: arg too long (%u) for buf %u — rejecting",
                 (unsigned)arg_len, (unsigned)out_cap);
        return false;
    }

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
    /* LwIP-mbox guard — see telegram_do_send_text for full rationale. */
    if (!wifi_sta_connected()) return false;
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
    /* LwIP-mbox guard — see telegram_do_send_text. */
    if (!wifi_sta_connected()) return false;
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

/* telegram_report_task removed 2026-05-12 — Google Sheets backend
 * dropped in favor of the on-device persistent audit ring (/log).
 * The HTTP GET helper that this task used is still available for
 * other callers if needed. */

static void telegram_send_snapshot_reply(const char *caption)
{
    /* Lazy camera init — first /photo wakes the sensor. */
    if (camera_ensure_initialized() != ESP_OK) {
        telegram_send_text(telegram_pick(
            "Camera failed to initialise. Check the ribbon and try /photo again.",
            "เริ่มต้นกล้องไม่สำเร็จ ตรวจสายแพรกล้องแล้วลอง /photo อีกครั้ง"));
        return;
    }
    // Tell the encoder a "client" is waiting so camera_task wakes up and
    // produces a fresh JPEG (camera_task otherwise skips work when
    // jpeg_enc_has_clients()==false). The 3 s timeout is long enough
    // for the camera to capture + JPEG-encode one frame even with the
    // 2-buffer pipeline.
    jpeg_enc_client_added();
    uint8_t *jpg_buf = NULL;
    size_t jpg_len = 0;
    esp_err_t got = jpeg_enc_get_frame(&jpg_buf, &jpg_len, 3000);
    jpeg_enc_client_removed();
    if (got == ESP_OK) {
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

    /* WiFi-readiness gate. Without this guard, calling esp_http_client_*
     * while STA is disconnected (or in mid-reconnect) reaches into the
     * LwIP TCP/IP thread mailbox which may have been torn down and
     * asserts inside tcpip_send_msg_wait_sem:449 "Invalid mbox" — that
     * brings down the WHOLE board, not just this task. Observed in
     * core-dump from 2026-05-12 (3 reboots/10 min). Queue the message
     * to offline_sync instead and exit cleanly; offline_sync drains it
     * when WiFi is back. */
    if (!wifi_sta_connected()) {
        ESP_LOGW(TAG, "WiFi not connected — deferring Telegram text to offline_sync");
        offline_sync_queue_telegram_text(args->message);
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
        /* Stack 10 KB → 16 KB: panic 2026-05-13 traced to TCB corruption
         * from stack overflow. Crash dump showed task name field zeroed
         * (was "tg_text_wrk") and xTaskPriorityDisinherit asserting that
         * the mutex holder ≠ current task — classic symptom of the
         * canary value at the bottom of the stack getting trampled.
         * The worker's hot path = esp_http_client_init + esp_http_client_perform
         * + TLS handshake via mbedtls + esp_crt_bundle + offline_sync
         * write — each is ~2 KB of stack, and the sum easily exceeds
         * 10 KB once nested into telegram_do_send_text's local buffers. */
        if (xTaskCreate(telegram_text_worker, "tg_text_wrk", 16384,
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

    /* When WiFi is down, every enqueue ultimately strdups the message,
     * fails xQueueSend, falls through to offline_sync which ALSO copies
     * the message into its NVS-backed queue. A flurry of low-pill
     * alerts during a long outage was eating heap until malloc returned
     * NULL → crash. Throttle to one offline persist per 30 s when STA
     * is disconnected; the user gets the next live alert as soon as
     * WiFi is back. */
    if (!wifi_sta_connected()) {
        static uint32_t s_last_offline_ms = 0;
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (s_last_offline_ms != 0 && (now_ms - s_last_offline_ms) < 30000) {
            ESP_LOGW(TAG, "Telegram enqueue dropped — WiFi offline, throttling");
            return;
        }
        s_last_offline_ms = (now_ms == 0) ? 1 : now_ms;
    }

    tg_task_args_t *args = malloc(sizeof(tg_task_args_t));
    if (!args) return;
    args->message = strdup(msg);
    if (!args->message) { free(args); return; }
    args->with_keyboard = with_keyboard;

    if (xQueueSend(s_tg_text_queue, &args, 0) != pdTRUE) {
        // Queue full — instead of silently dropping, persist the message
        // through offline_sync so the next time the worker drains it can
        // get sent. Caller doesn't lose a low-stock alert just because
        // 16 other messages stacked up at the same moment.
        ESP_LOGW(TAG, "Telegram text queue full — falling back to offline_sync");
        offline_sync_queue_telegram_text(args->message);
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
    /* Same WiFi-readiness gate as telegram_do_send_text — calling
     * esp_http_client_* with STA disconnected asserts inside LwIP's
     * tcpip_send_msg_wait_sem and reboots the board. Drop the photo
     * silently (offline_sync doesn't carry photos) but log it. */
    if (!wifi_sta_connected()) {
        ESP_LOGW(TAG, "WiFi not connected — dropping Telegram photo");
        free(args->photo_buf);
        free(args->caption);
        free(args);
        vTaskDelete(NULL);
        return;
    }
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
        
        // JPEG buffer is huge (~100KB+), esp_http_client_write will drop
        // data if we don't loop it. Bail on negative return (socket died)
        // or after 5 consecutive zero-length writes (TCP send buffer
        // truly stuck). Footer is gated on total_written==photo_len at
        // line 990, so partial writes never produce a corrupt multipart.
        int total_written = 0;
        int zero_streak = 0;
        bool socket_err = false;
        while (total_written < (int)args->photo_len) {
            int w = esp_http_client_write(client,
                                          (const char *)args->photo_buf + total_written,
                                          args->photo_len - total_written);
            if (w < 0) {
                ESP_LOGE(TAG, "Photo write socket error at offset %d (errno err=%d)",
                         total_written, w);
                socket_err = true;
                break;
            }
            if (w == 0) {
                if (++zero_streak >= 5) {
                    ESP_LOGE(TAG, "Photo write stuck (5 consecutive zero writes) at offset %d",
                             total_written);
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            total_written += w;
            zero_streak = 0;
        }
        (void)socket_err;
        
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
    portENTER_CRITICAL(&s_photo_inflight_mux);
    if (s_photo_inflight > 0) s_photo_inflight--;
    portEXIT_CRITICAL(&s_photo_inflight_mux);
    vTaskDelete(NULL);
}

void telegram_send_photo_with_text(uint8_t *photo_buf, size_t photo_len, const char *caption) {
    if (!photo_buf || photo_len == 0 || !cloud_secrets_has_telegram()) {
        free(photo_buf);
        return;
    }

    portENTER_CRITICAL(&s_photo_inflight_mux);
    bool over_limit = (s_photo_inflight >= TG_PHOTO_MAX_INFLIGHT);
    if (!over_limit) s_photo_inflight++;
    portEXIT_CRITICAL(&s_photo_inflight_mux);
    if (over_limit) {
        ESP_LOGW(TAG, "Photo backpressure: %d in-flight, dropping new send",
                 TG_PHOTO_MAX_INFLIGHT);
        free(photo_buf);
        return;
    }

    tg_photo_args_t *args = malloc(sizeof(tg_photo_args_t));
    if (!args) {
        free(photo_buf); // Take ownership of buffer to free it
        portENTER_CRITICAL(&s_photo_inflight_mux);
        if (s_photo_inflight > 0) s_photo_inflight--;
        portEXIT_CRITICAL(&s_photo_inflight_mux);
        return;
    }

    args->photo_buf = photo_buf;
    args->photo_len = photo_len;
    args->caption = strdup(caption ? caption : "");

    if (!args->caption) {
        free(photo_buf);
        free(args);
        portENTER_CRITICAL(&s_photo_inflight_mux);
        if (s_photo_inflight > 0) s_photo_inflight--;
        portEXIT_CRITICAL(&s_photo_inflight_mux);
        return;
    }

    if (xTaskCreate(telegram_send_photo_task, "tg_pho_task", 12288, args, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Telegram photo task");
        free(args->photo_buf);
        free(args->caption);
        free(args);
        portENTER_CRITICAL(&s_photo_inflight_mux);
        if (s_photo_inflight > 0) s_photo_inflight--;
        portEXIT_CRITICAL(&s_photo_inflight_mux);
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

        char msg[2048];
        int len = snprintf(msg, sizeof(msg), "%s",
                           telegram_pick("Medication status for all 6 modules\n\n",
                                         "สถานะยาทั้ง 6 โมดูล\n\n"));

        for (int i = 0; i < DISPENSER_MED_COUNT && len < (int)sizeof(msg) - 1; i++) {
            bool th = (s_tg_language == TELEGRAM_LANG_TH);
            const char *name = (sh->med[i].name[0])
                                   ? sh->med[i].name
                                   : (th ? "(ยังไม่ได้ตั้งชื่อ)" : "(unnamed)");
            int written = snprintf(msg + len, sizeof(msg) - (size_t)len,
                                   th ? "โมดูล %d: %s\nคงเหลือ: %d เม็ด\n\n"
                                      : "Module %d: %s\nRemaining: %d pills\n\n",
                                   i + 1, name, sh->med[i].count);
            if (written < 0) break;
            if (written >= (int)(sizeof(msg) - (size_t)len)) {
                len = (int)sizeof(msg) - 1;
                break;
            }
            len += written;
        }

        telegram_send_text(msg);
    } else if (strcmp(cmd, "/log") == 0 || strcmp(cmd, "/history") == 0) {
        /* /report removed 2026-05-12 — Google Sheets dependency dropped.
         * Use /log instead (persistent on-device, up to 256 events). */
        /* Detailed activity log from the persistent audit ring —
         * up to 256 most-recent events (≈ 2-3 months of typical use),
         * each with full DD/MM HH:MM timestamp. Stored in NVS so it
         * survives reboots. The device is the source of truth — no
         * Google Sheets dependency. */
        size_t total = dispenser_audit_count();
        bool th = (s_tg_language == TELEGRAM_LANG_TH);
        if (total == 0) {
            telegram_send_text(th ? "ยังไม่มีประวัติการจ่ายยา"
                                  : "No dose history yet.");
            return;
        }

        /* Fetch into a heap buffer — 256 * sizeof(entry) ≈ 3 KB which
         * is too big for the BSS stack of this handler. */
        dispenser_audit_entry_t *entries =
            (dispenser_audit_entry_t *)calloc(total, sizeof(*entries));
        if (!entries) {
            telegram_send_text(th ? "หน่วยความจำไม่พอสำหรับดึงประวัติ"
                                  : "Out of memory while fetching log.");
            return;
        }
        size_t n = dispenser_audit_get(entries, total);
        const netpie_shadow_t *sh = netpie_get_shadow();

        /* Audit ring now only stores scheduled-dispense outcomes
         * ('S'), so visible_n == n. Loop kept for forward-compat in
         * case a non-'S' source ever sneaks in. */
        size_t visible_n = 0;
        for (size_t i = 0; i < n; ++i) {
            char src = entries[i].source;
            if (src != 'V' && src != 'W') ++visible_n;
        }
        if (visible_n == 0) {
            telegram_send_text(th ? "ยังไม่มีประวัติการจ่ายยา"
                                  : "No dose history yet.");
            free(entries);
            return;
        }

        /* Telegram caps each message at 4096 chars — flush whenever the
         * buffer crosses ~3500 so we always have headroom for the next
         * entry + trailing footer. The header is reprinted on each
         * chunk so the recipient knows the chunks belong together. */
        const int FLUSH_AT = 3500;
        char msg[4000];
        int chunk_idx = 0;
        int len = snprintf(msg, sizeof(msg),
                           th ? "📋 ประวัติการจ่ายยา (ทั้งหมด %u รายการ)\n\n"
                              : "📋 Dose history (%u entries total)\n\n",
                           (unsigned)visible_n);

        for (size_t i = 0; i < n; ++i) {
            const dispenser_audit_entry_t *e = &entries[i];
            /* Only scheduled-dispense entries ('S') are written to the
             * ring now; this filter is a forward-compat safety net. */
            if (e->source == 'V' || e->source == 'W') continue;

            char ts[24] = "--/-- --:--";
            if (e->timestamp > 0) {
                time_t t = (time_t)e->timestamp;
                struct tm tmv = {0};
                if (localtime_r(&t, &tmv) && tmv.tm_year >= 100) {
                    strftime(ts, sizeof(ts), th ? "%d/%m/%y %H:%M" : "%d %b %y %H:%M", &tmv);
                }
            }
            const char *name = (e->med_idx >= 0 && e->med_idx < DISPENSER_MED_COUNT &&
                                sh->med[e->med_idx].name[0])
                                   ? sh->med[e->med_idx].name
                                   : (th ? "(ไม่ทราบชื่อ)" : "(unnamed)");
            const char *what;
            const char *icon;
            switch (e->source) {
                case 'S':
                    icon = "✅"; what = th ? "จ่ายยาสำเร็จ" : "Dispensed";
                    break;
                case 'M':
                    icon = "✅"; what = th ? "จ่าย/คืนยาแมนวล" : "Manual";
                    break;
                case 'L':
                    icon = "⏳"; what = th ? "พลาดยามื้อนั้น" : "Missed dose";
                    break;
                case 'N':
                    icon = "🔴"; what = th ? "ยาหมด ไม่ได้จ่าย" : "Out of stock — skipped";
                    break;
                default:
                    icon = "•";  what = th ? "อื่นๆ" : "Other";
                    break;
            }
            int written;
            if (e->source == 'L' || e->source == 'N') {
                /* No count change to display for missed / out-of-stock. */
                written = snprintf(msg + len, sizeof(msg) - (size_t)len,
                                   th ? "%s %s — โมดูล %d (%s)\n   %s\n\n"
                                      : "%s %s — Module %d (%s)\n   %s\n\n",
                                   icon, ts, e->med_idx + 1, name, what);
            } else {
                int delta = (int)e->to_count - (int)e->from_count;
                written = snprintf(msg + len, sizeof(msg) - (size_t)len,
                                   th ? "%s %s — โมดูล %d (%s)\n   %s: %d → %d (%s%d)\n\n"
                                      : "%s %s — Module %d (%s)\n   %s: %d → %d (%s%d)\n\n",
                                   icon, ts, e->med_idx + 1, name, what,
                                   e->from_count, e->to_count,
                                   delta >= 0 ? "+" : "", delta);
            }
            if (written < 0) break;
            if (written >= (int)(sizeof(msg) - (size_t)len)) {
                /* Entry too big to fit — shouldn't happen, but bail
                 * gracefully. */
                break;
            }
            len += written;

            if (len >= FLUSH_AT && i + 1 < n) {
                telegram_send_text(msg);
                ++chunk_idx;
                len = snprintf(msg, sizeof(msg),
                               th ? "📋 (ต่อ %d)\n\n" : "📋 (cont. %d)\n\n",
                               chunk_idx + 1);
                /* Small spacing delay so Telegram delivers in order. */
                vTaskDelay(pdMS_TO_TICKS(300));
            }
        }

        telegram_send_text(msg);
        free(entries);
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
    } else if (strcmp(cmd, "/stop") == 0) {
        dispenser_emergency_set();
        telegram_send_text(telegram_pick(
            "EMERGENCY STOP set. No dispense will fire until /resume.",
            "หยุดฉุกเฉินแล้ว เครื่องจะไม่จ่ายยาจนกว่าจะกด /resume"));
    } else if (strcmp(cmd, "/resume") == 0) {
        dispenser_emergency_clear();
        telegram_send_text(telegram_pick(
            "Emergency stop cleared. Dispenser is back online.",
            "ยกเลิกหยุดฉุกเฉินแล้ว เครื่องจ่ายยากลับมาทำงานปกติ"));
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
            "- Medication Status (/status)\n"
            "- Dose History (/log)\n"
            "- Live Photo (/photo)\n"
            "- Help\n\n"
            "You can still use /lang en or /lang th anytime.",
            "บอทเครื่องจ่ายยาพร้อมใช้งาน\n\n"
            "กดปุ่มเมนูด้านล่างเพื่อใช้งานได้ง่ายที่สุด\n"
            "- สถานะยา (/status)\n"
            "- ประวัติการจ่ายยา (/log)\n"
            "- ถ่ายภาพล่าสุด (/photo)\n"
            "- Help\n\n"
            "ถ้าต้องการเปลี่ยนภาษา ใช้ /lang en หรือ /lang th"));
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
