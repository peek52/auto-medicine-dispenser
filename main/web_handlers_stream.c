// ─────────────────────────────────────────────────────────────────────────────
//  web_handlers_stream.c — Camera MJPEG stream + snapshot handlers
//  Routes: GET /stream (port 81), GET /capture (port 81)
// ─────────────────────────────────────────────────────────────────────────────

#include "web_handlers_stream.h"
#include "web_handlers_status.h"
#include "camera_init.h"
#include "jpeg_encoder.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "web_stream";
static int s_stream_delay_ms = 0;

static int clamp_stream_delay_ms(int delay_ms) {
    if (delay_ms < 0) return 0;
    if (delay_ms > 300) return 300;
    return delay_ms;
}

int camera_stream_get_delay_ms(void) {
    return s_stream_delay_ms;
}

void camera_stream_set_delay_ms(int delay_ms) {
    s_stream_delay_ms = clamp_stream_delay_ms(delay_ms);
}

/* ── MJPEG multipart boundary strings ── */
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART         = "Content-Type: image/jpeg\r\nContent-Length: %10u\r\n\r\n";

/* ── GET /stream — MJPEG stream แบบ infinite loop ── */
esp_err_t stream_handler(httpd_req_t *req) {
    esp_err_t auth = web_require_tech_api_auth(req);
    if (auth != ESP_OK) return auth;

    ESP_LOGI(TAG, "MJPEG client connected");
    httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char part_hdr[128];
    while (true) {
        uint8_t *jpeg_buf = NULL;
        size_t jpeg_len = 0;

        /* รอ JPEG frame จาก encoder (timeout 1 วินาที) */
        if (jpeg_enc_get_frame(&jpeg_buf, &jpeg_len, 1000) != ESP_OK) continue;

        /* ส่ง boundary */
        if (httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY)) != ESP_OK) {
            jpeg_enc_release_frame();
            break;
        }

        /* ส่ง Content-Type header + JPEG data */
        int hdr_len = snprintf(part_hdr, sizeof(part_hdr), _STREAM_PART, (unsigned)jpeg_len);
        if (httpd_resp_send_chunk(req, part_hdr, hdr_len) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)jpeg_buf, jpeg_len) != ESP_OK) {
            jpeg_enc_release_frame();
            break;
        }
        jpeg_enc_release_frame();
        if (s_stream_delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(s_stream_delay_ms));
        }
    }
    ESP_LOGI(TAG, "MJPEG client disconnected");
    return ESP_OK;
}

/* ── GET /capture — ถ่ายภาพ JPEG เดี่ยว (snapshot) ── */
esp_err_t capture_handler(httpd_req_t *req) {
    esp_err_t auth = web_require_tech_api_auth(req);
    if (auth != ESP_OK) return auth;

    uint8_t *jpeg_buf = NULL;
    size_t jpeg_len = 0;
    if (jpeg_enc_get_frame(&jpeg_buf, &jpeg_len, 1000) != ESP_OK) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_send(req, (const char *)jpeg_buf, jpeg_len);
    jpeg_enc_release_frame();
    return ret;
}

static bool query_value_int(httpd_req_t *req, const char *key, int *out_value) {
    char query[160];
    char param[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
    if (httpd_query_key_value(query, key, param, sizeof(param)) != ESP_OK) return false;
    *out_value = atoi(param);
    return true;
}

esp_err_t camera_state_handler(httpd_req_t *req) {
    esp_err_t auth = web_require_tech_api_auth(req);
    if (auth != ESP_OK) return auth;

    char json[256];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"sensor\":\"%s\",\"jpeg_quality\":%d,"
             "\"stream_delay_ms\":%d,\"mirror\":%d,\"vflip\":%d}",
             camera_get_sensor_name(),
             jpeg_enc_get_quality(),
             camera_stream_get_delay_ms(),
             camera_get_hmirror() ? 1 : 0,
             camera_get_vflip() ? 1 : 0);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, json, strlen(json));
}

esp_err_t camera_set_handler(httpd_req_t *req) {
    esp_err_t auth = web_require_tech_api_auth(req);
    if (auth != ESP_OK) return auth;

    int value = 0;
    if (query_value_int(req, "quality", &value)) {
        jpeg_enc_set_quality(value);
    }
    if (query_value_int(req, "delay", &value)) {
        camera_stream_set_delay_ms(value);
    }
    if (query_value_int(req, "mirror", &value)) {
        esp_err_t ret = camera_set_hmirror(value != 0);
        if (ret != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mirror unsupported");
        }
    }
    if (query_value_int(req, "vflip", &value)) {
        esp_err_t ret = camera_set_vflip(value != 0);
        if (ret != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "vflip unsupported");
        }
    }

    return camera_state_handler(req);
}
