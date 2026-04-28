#pragma once

#include "esp_http_server.h"
#include "esp_err.h"

esp_err_t tech_dashboard_handler(httpd_req_t *req);
esp_err_t tech_reboot_handler(httpd_req_t *req);
esp_err_t tech_estop_handler(httpd_req_t *req);
esp_err_t tech_quiet_handler(httpd_req_t *req);
