#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  web_handlers_servo.h — Servo (PCA9685) HTTP API handlers
//  Routes: /servo/home, /servo/work, /servo/test, /servo/set, /servo/state
// ─────────────────────────────────────────────────────────────────────────────

#include "esp_http_server.h"
#include "esp_err.h"

esp_err_t servo_home_handler (httpd_req_t *req);
esp_err_t servo_work_handler (httpd_req_t *req);
esp_err_t servo_test_handler (httpd_req_t *req);
esp_err_t servo_set_handler  (httpd_req_t *req);
esp_err_t servo_state_handler(httpd_req_t *req);
