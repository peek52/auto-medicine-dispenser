#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t jpeg_enc_init(int width, int height);
esp_err_t jpeg_enc_encode_frame(const uint8_t *yuv422_data, size_t yuv422_len);
esp_err_t jpeg_enc_get_frame(uint8_t **out_buf, size_t *out_len, uint32_t timeout_ms);
void      jpeg_enc_release_frame(void);
esp_err_t jpeg_enc_set_quality(int quality);
int       jpeg_enc_get_quality(void);

/* Streaming clients track — when zero clients are watching the camera,
 * the encoder skips work. Increment on /stream connect, decrement on
 * disconnect; MJPEG handler does this in web_handlers_stream.c. */
void      jpeg_enc_client_added(void);
void      jpeg_enc_client_removed(void);
bool      jpeg_enc_has_clients(void);

#ifdef __cplusplus
}
#endif
