#pragma once
#include "esp_err.h"
#include <stdint.h>
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

#ifdef __cplusplus
}
#endif
