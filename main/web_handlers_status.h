#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  web_handlers_status.h — System status JSON + WiFi setup handlers
// ─────────────────────────────────────────────────────────────────────────────

#include "esp_http_server.h"
#include "esp_err.h"
#include <stdbool.h>

esp_err_t status_json_handler(httpd_req_t *req);
esp_err_t web_entry_handler(httpd_req_t *req);
esp_err_t wifi_setup_handler(httpd_req_t *req);

/* ── POST /wifi/save — บันทึก SSID/Pass แล้ว restart ── */
esp_err_t wifi_save_handler(httpd_req_t *req);

/* ── GET /wifi/scan — สแกนเครือข่าย WiFi ── */
esp_err_t wifi_scan_handler(httpd_req_t *req);

/* ── POST /wifi/forget — ล้างค่ารหัสและลบ WiFi ── */
esp_err_t wifi_forget_handler(httpd_req_t *req);

esp_err_t cloud_setup_handler(httpd_req_t *req);
esp_err_t cloud_save_handler(httpd_req_t *req);
esp_err_t cloud_test_handler(httpd_req_t *req);
esp_err_t access_state_handler(httpd_req_t *req);
esp_err_t access_save_handler(httpd_req_t *req);
esp_err_t cloud_login_handler(httpd_req_t *req);
esp_err_t cloud_logout_handler(httpd_req_t *req);
esp_err_t maintenance_unlock_handler(httpd_req_t *req);
bool web_auth_cookie_valid(httpd_req_t *req);
bool web_tech_cookie_valid(httpd_req_t *req);
esp_err_t web_require_maintenance_auth(httpd_req_t *req);
esp_err_t web_require_maintenance_api_auth(httpd_req_t *req);
esp_err_t web_require_tech_page_auth(httpd_req_t *req);
esp_err_t web_require_tech_api_auth(httpd_req_t *req);
esp_err_t web_redirect_to_cloud_login(httpd_req_t *req, bool clear_session);
esp_err_t sound_state_handler(httpd_req_t *req);
esp_err_t sound_save_handler(httpd_req_t *req);
esp_err_t sound_play_handler(httpd_req_t *req);

/* ── GET /sensors.json — ค่า VL53 ทั้ง 6 channel ── */
esp_err_t sensors_json_handler(httpd_req_t *req);

/* ── POST /sensors/config — ตั้งค่า tray config ต่อ channel ── */
esp_err_t sensors_config_handler(httpd_req_t *req);

/* ── POST /sensors/cal_capture — บันทึกระยะปัจจุบัน (full/empty) ── */
esp_err_t sensors_capture_handler(httpd_req_t *req);

/* ── GET /audit.json — ประวัติการเพิ่ม/ลด/จ่ายของยาแต่ละช่อง ── */
esp_err_t audit_json_handler(httpd_req_t *req);

/* /sensors HTML page removed — /tech "ดูเซ็นเซอร์" tab is the canonical view. */
