// ─────────────────────────────────────────────────────────────────────────────
//  web_handlers_servo.c — Servo (PCA9685) HTTP API handlers
//  Routes: GET /servo/home, /servo/work, /servo/test, /servo/set, /servo/state
// ─────────────────────────────────────────────────────────────────────────────

#include "web_handlers_servo.h"
#include "web_handlers_status.h"
#include "pca9685.h"
#include "config.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web_servo";

/* ── Query helper: อ่านค่า ?ch=N จาก URL ── */
static int get_ch(httpd_req_t *req) {
    char buf[64];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(buf, "ch", param, sizeof(param)) == ESP_OK) {
            return atoi(param);
        }
    }
    return 0;
}

/* ── GET /servo/home?ch=N — ส่ง servo ไปตำแหน่ง Home ── */
esp_err_t servo_home_handler(httpd_req_t *req) {
    esp_err_t auth = web_require_tech_api_auth(req);
    if (auth != ESP_OK) return auth;

    int ch = get_ch(req);
    esp_err_t err = pca9685_go_home(ch);
    char r[128];
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ch%d HOME failed: %s", ch, esp_err_to_name(err));
        snprintf(r, sizeof(r), "{\"ok\":false,\"ch\":%d,\"error\":\"%s\"}",
                 ch, esp_err_to_name(err));
        return httpd_resp_send(req, r, strlen(r));
    }
    ESP_LOGD(TAG, "ch%d -> HOME", ch);
    snprintf(r, sizeof(r), "{\"ok\":true,\"ch\":%d}", ch);
    return httpd_resp_send(req, r, strlen(r));
}

/* ── GET /servo/work?ch=N — ส่ง servo ไปตำแหน่ง Work ── */
esp_err_t servo_work_handler(httpd_req_t *req) {
    esp_err_t auth = web_require_tech_api_auth(req);
    if (auth != ESP_OK) return auth;

    int ch = get_ch(req);
    esp_err_t err = pca9685_go_work(ch);
    char r[128];
    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ch%d WORK failed: %s", ch, esp_err_to_name(err));
        snprintf(r, sizeof(r), "{\"ok\":false,\"ch\":%d,\"error\":\"%s\"}",
                 ch, esp_err_to_name(err));
        return httpd_resp_send(req, r, strlen(r));
    }
    ESP_LOGD(TAG, "ch%d -> WORK", ch);
    snprintf(r, sizeof(r), "{\"ok\":true,\"ch\":%d}", ch);
    return httpd_resp_send(req, r, strlen(r));
}

/* ── Test task: Work → รอ 700ms → Home (background) ── */
static void servo_test_task(void *arg) {
    int ch = (int)(intptr_t)arg;
    esp_err_t e1 = pca9685_go_work(ch);
    if (e1 != ESP_OK) ESP_LOGE(TAG, "test ch%d WORK failed: %s", ch, esp_err_to_name(e1));
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_err_t e2 = pca9685_go_home(ch);
    if (e2 != ESP_OK) ESP_LOGE(TAG, "test ch%d HOME failed: %s", ch, esp_err_to_name(e2));
    vTaskDelete(NULL);
}

/* ── GET /servo/test?ch=N — ทดสอบ servo (Work แล้วกลับ Home) ── */
esp_err_t servo_test_handler(httpd_req_t *req) {
    esp_err_t auth = web_require_tech_api_auth(req);
    if (auth != ESP_OK) return auth;

    int ch = get_ch(req);
    /* spawn background task เพื่อไม่บล็อก httpd → MJPEG stream ไม่หลุด
     * Stack 2 KB → 4 KB: pca9685_go_work/home fan out through ESP_LOGE
     * (esp_err_to_name(...)) on errors — log macro alone needs ~300 B
     * and a back-to-back I2C-failure path left 2 KB uncomfortably tight. */
    if (xTaskCreate(servo_test_task, "sv_test", 4096, (void *)(intptr_t)ch, 5, NULL) != pdPASS) {
        return httpd_resp_send_500(req);
    }
    char r[64];
    snprintf(r, sizeof(r), "{\"ok\":true,\"ch\":%d,\"action\":\"test\"}", ch);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, r, strlen(r));
}

/* ── GET /servo/set?ch=N&home=A&work=B — ตั้งค่ามุม Home/Work ── */
esp_err_t servo_set_handler(httpd_req_t *req) {
    esp_err_t auth = web_require_tech_api_auth(req);
    if (auth != ESP_OK) return auth;

    char buf[128];
    int ch = 0, h = 66, w = 33;
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(buf, "ch",   param, sizeof(param)) == ESP_OK) ch = atoi(param);
        if (httpd_query_key_value(buf, "home", param, sizeof(param)) == ESP_OK) h  = atoi(param);
        if (httpd_query_key_value(buf, "work", param, sizeof(param)) == ESP_OK) w  = atoi(param);
    }
    
    // ตั้งค่าใน RAM คืนนี้
    pca9685_set_positions(ch, h, w);
    
    // บันทึกลง NVS ให้จำค่าไว้ถาวรข้ามรอบการบูต
    extern void pca9685_save_nvs(void); // Define here to avoid needing h change for now
    pca9685_save_nvs();

    char r[64];
    snprintf(r, sizeof(r), "{\"ok\":true,\"ch\":%d,\"home\":%d,\"work\":%d}", ch, h, w);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, r, strlen(r));
}

/* ── GET /servo/state — คืน JSON สถานะ servo ทุก channel ── */
esp_err_t servo_state_handler(httpd_req_t *req) {
    esp_err_t auth = web_require_maintenance_auth(req);
    if (auth != ESP_OK) return auth;

    char j[640];
    int pos = snprintf(j, sizeof(j), "{\"channels\":[");
    for (int i = 0; i < SERVO_NUM_CHANNELS; i++) {
        pos += snprintf(j + pos, sizeof(j) - pos,
                        "%s{\"ch\":%d,\"home\":%d,\"work\":%d,\"cur\":%d}",
                        i == 0 ? "" : ",",
                        i,
                        g_servo[i].home_angle,
                        g_servo[i].work_angle,
                        g_servo[i].cur_angle);
    }
    snprintf(j + pos, sizeof(j) - pos, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, j, strlen(j));
}
