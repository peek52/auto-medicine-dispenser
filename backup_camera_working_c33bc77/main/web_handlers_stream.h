#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  web_handlers_stream.h — Camera MJPEG stream + snapshot handlers
//  Routes: /stream (port 81), /capture (port 81)
// ─────────────────────────────────────────────────────────────────────────────

#include "esp_http_server.h"
#include "esp_err.h"

esp_err_t stream_handler (httpd_req_t *req);
esp_err_t capture_handler(httpd_req_t *req);
esp_err_t camera_state_handler(httpd_req_t *req);
esp_err_t camera_set_handler(httpd_req_t *req);
int camera_stream_get_delay_ms(void);
void camera_stream_set_delay_ms(int delay_ms);
