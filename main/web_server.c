// ─────────────────────────────────────────────────────────────────────────────
//  web_server.c — HTTP Server init + route registration
//
//  Port 80  → dashboard + API (index, status.json, servo/*, wifi/*)
//  Port 81  → MJPEG stream only (/stream, /capture)
//
//  Handlers แยกตามหน้าที่:
//    index_html.h           ← UI / HTML / CSS / JS
//    web_handlers_servo.c   ← Servo API
//    web_handlers_status.c  ← status JSON + WiFi setup
//    web_handlers_stream.c  ← MJPEG stream + snapshot
// ─────────────────────────────────────────────────────────────────────────────

#include "web_server.h"
#include "index_html.h"
#include "web_handlers_servo.h"
#include "web_handlers_status.h"
#include "web_handlers_stream.h"
#include "web_handlers_tech.h"
#include "web_log.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "web_server";

/* ── GET / — ส่งหน้า Dashboard HTML ── */
static esp_err_t index_handler(httpd_req_t *req) {
    return web_entry_handler(req);
}

static esp_err_t maint_handler(httpd_req_t *req) {
    esp_err_t auth = web_require_tech_page_auth(req);
    if (auth != ESP_OK) return auth;

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html, strlen(index_html));
}

static esp_err_t monitor_handler(httpd_req_t *req) {
    esp_err_t auth = web_require_maintenance_auth(req);
    if (auth != ESP_OK) return auth;

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html, strlen(index_html));
}

/* ─────────────────────────────────────────────────────────────────────────────
   Port 80 — API Server (dashboard + REST endpoints)
───────────────────────────────────────────────────────────────────────────── */
httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers   = 36;
    config.max_open_sockets   = 12;  /* ป้องกัน socket เต็มตอน polling */
    config.lru_purge_enable   = true;
    config.recv_wait_timeout  = 3;   /* วินาที — ปิด stale connections เร็ว */
    config.send_wait_timeout  = 3;
    config.stack_size         = 8192;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Error starting port-80 server");
        return NULL;
    }

    /* ── Register Routes ── */
    httpd_uri_t routes[] = {
        { "/",              HTTP_GET,  index_handler,        NULL },
        { "/monitor",       HTTP_GET,  monitor_handler,      NULL },
        { "/maint",         HTTP_GET,  maint_handler,        NULL },
        { "/maint/unlock",  HTTP_POST, maintenance_unlock_handler, NULL },
        { "/logs/tail",     HTTP_GET,  web_log_tail_handler, NULL },
        { "/status.json",  HTTP_GET,  status_json_handler,  NULL },
        { "/camera/state", HTTP_GET,  camera_state_handler, NULL },
        { "/camera/set",   HTTP_GET,  camera_set_handler,   NULL },
        { "/servo/home",   HTTP_GET,  servo_home_handler,   NULL },
        { "/servo/work",   HTTP_GET,  servo_work_handler,   NULL },
        { "/servo/test",   HTTP_GET,  servo_test_handler,   NULL },
        { "/servo/set",    HTTP_GET,  servo_set_handler,    NULL },
        { "/servo/state",  HTTP_GET,  servo_state_handler,  NULL },
        { "/wifi",         HTTP_GET,  wifi_setup_handler,   NULL },
        { "/wifi/save",    HTTP_POST, wifi_save_handler,    NULL },
        { "/wifi/scan",    HTTP_GET,  wifi_scan_handler,    NULL },
        { "/wifi/forget",  HTTP_POST, wifi_forget_handler,  NULL },
        { "/cloud",        HTTP_GET,  cloud_setup_handler,  NULL },
        { "/cloud/login",  HTTP_POST, cloud_login_handler,  NULL },
        { "/cloud/logout", HTTP_GET,  cloud_logout_handler, NULL },
        { "/cloud/save",   HTTP_POST, cloud_save_handler,   NULL },
        { "/cloud/test",   HTTP_GET,  cloud_test_handler,   NULL },
        { "/access/state", HTTP_GET,  access_state_handler, NULL },
        { "/access/save",  HTTP_POST, access_save_handler,  NULL },
        { "/sound/config", HTTP_GET,  sound_state_handler,  NULL },
        { "/sound/save",   HTTP_POST, sound_save_handler,   NULL },
        { "/sound/play",   HTTP_GET,  sound_play_handler,   NULL },
        { "/sensors",         HTTP_GET,  sensors_page_handler,   NULL },
        { "/sensors.json",    HTTP_GET,  sensors_json_handler,   NULL },
        { "/sensors/config",  HTTP_POST, sensors_config_handler, NULL },
        { "/tech",            HTTP_GET,  tech_dashboard_handler, NULL },
        { "/tech/reboot",     HTTP_POST, tech_reboot_handler,    NULL },
    };

    for (int i = 0; i < sizeof(routes)/sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }

    ESP_LOGI(TAG, "API server started on port 80");
    return server;
}

/* ─────────────────────────────────────────────────────────────────────────────
   Port 81 — Stream Server (MJPEG only)
───────────────────────────────────────────────────────────────────────────── */
httpd_handle_t start_stream_server(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port       = 81;
    config.ctrl_port         = 32769;
    config.max_uri_handlers  = 2;
    config.max_open_sockets  = 4;   /* stream ต้องการ sockets น้อย */
    config.lru_purge_enable  = true;
    config.recv_wait_timeout = 10;  /* long timeout — MJPEG เป็น persistent connection */
    config.send_wait_timeout = 10;
    config.stack_size        = 8192;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Error starting port-81 stream server");
        return NULL;
    }

    httpd_uri_t u_cam = { "/stream",  HTTP_GET, stream_handler,  NULL };
    httpd_uri_t u_cap = { "/capture", HTTP_GET, capture_handler, NULL };
    httpd_register_uri_handler(server, &u_cam);
    httpd_register_uri_handler(server, &u_cap);

    ESP_LOGI(TAG, "Stream server started on port 81");
    return server;
}
