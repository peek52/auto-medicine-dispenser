#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

void web_log_init(void);
esp_err_t web_log_tail_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
