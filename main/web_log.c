#include "web_log.h"

#include "web_handlers_status.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define WEB_LOG_BUF_SIZE 12288
#define WEB_LOG_TMP_SIZE 384

static char s_log_buf[WEB_LOG_BUF_SIZE];
static size_t s_log_len = 0;
static bool s_log_wrapped = false;
static SemaphoreHandle_t s_log_mutex = NULL;
static vprintf_like_t s_prev_vprintf = NULL;

static void web_log_append_locked(const char *text, size_t len) {
    if (!text || !len) return;

    if (len >= WEB_LOG_BUF_SIZE - 1) {
        text += (len - (WEB_LOG_BUF_SIZE - 2));
        len = WEB_LOG_BUF_SIZE - 2;
    }

    if (s_log_len + len >= WEB_LOG_BUF_SIZE) {
        size_t drop = (s_log_len + len) - (WEB_LOG_BUF_SIZE - 1);
        if (drop > s_log_len) drop = s_log_len;
        memmove(s_log_buf, s_log_buf + drop, s_log_len - drop);
        s_log_len -= drop;
        s_log_wrapped = true;
    }

    memcpy(s_log_buf + s_log_len, text, len);
    s_log_len += len;
    s_log_buf[s_log_len] = '\0';
}

static int web_log_vprintf(const char *fmt, va_list args) {
    va_list args_copy;
    va_copy(args_copy, args);

    int written = s_prev_vprintf ? s_prev_vprintf(fmt, args) : vprintf(fmt, args);

    char tmp[WEB_LOG_TMP_SIZE];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args_copy);
    va_end(args_copy);

    if (n > 0 && s_log_mutex && xSemaphoreTake(s_log_mutex, 0) == pdTRUE) {
        size_t append_len = (size_t)n;
        if (append_len >= sizeof(tmp)) append_len = sizeof(tmp) - 1;
        web_log_append_locked(tmp, append_len);
        xSemaphoreGive(s_log_mutex);
    }

    return written;
}

void web_log_init(void) {
    if (!s_log_mutex) {
        s_log_mutex = xSemaphoreCreateMutex();
    }
    if (!s_prev_vprintf) {
        s_prev_vprintf = esp_log_set_vprintf(web_log_vprintf);
    }
}

esp_err_t web_log_tail_handler(httpd_req_t *req) {
    esp_err_t auth = web_require_maintenance_api_auth(req);
    if (auth != ESP_OK) return auth;

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (!s_log_mutex || xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return httpd_resp_sendstr(req, "Log buffer busy");
    }

    if (s_log_wrapped) {
        httpd_resp_send_chunk(req, "[live log ring buffer active]\n", HTTPD_RESP_USE_STRLEN);
    }
    if (s_log_len > 0) {
        httpd_resp_send_chunk(req, s_log_buf, s_log_len);
    } else {
        httpd_resp_send_chunk(req, "No logs yet.", HTTPD_RESP_USE_STRLEN);
    }

    xSemaphoreGive(s_log_mutex);
    return httpd_resp_send_chunk(req, NULL, 0);
}
