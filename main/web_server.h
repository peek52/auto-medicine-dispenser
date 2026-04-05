#pragma once
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start HTTP web server on port 80 (API + dashboard) */
httpd_handle_t start_webserver(void);

/** Start MJPEG stream server on port 81 (stream only, separate task) */
httpd_handle_t start_stream_server(void);

#ifdef __cplusplus
}
#endif
