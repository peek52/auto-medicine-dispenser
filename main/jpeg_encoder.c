#include "jpeg_encoder.h"
#include "driver/jpeg_encode.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "jpeg_enc";

static jpeg_encoder_handle_t s_jpeg_handle = NULL;
static int s_width = 0;
static int s_height = 0;

#define JPEG_BUFS 3
static uint8_t *s_jpeg_buf[JPEG_BUFS] = {NULL, NULL, NULL};
static size_t s_jpeg_buf_size = 0;
static size_t s_jpeg_len[JPEG_BUFS] = {0, 0, 0};
static volatile int s_write_idx = 0;
static volatile int s_read_idx = -1;
static volatile int s_active_read_idx = -1;
static int s_jpeg_quality = 60;

static SemaphoreHandle_t s_swap_mutex = NULL;
static SemaphoreHandle_t s_frame_ready = NULL;
static SemaphoreHandle_t s_read_mutex = NULL;

static int clamp_jpeg_quality(int quality) {
    if (quality < 20) return 20;
    if (quality > 90) return 90;
    return quality;
}

esp_err_t jpeg_enc_init(int width, int height) {
    s_width = width;
    s_height = height;

    jpeg_encode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 3000,  // was 100 — too small, caused DMA2D assert crash
    };
    esp_err_t ret = jpeg_new_encoder_engine(&eng_cfg, &s_jpeg_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create JPEG encoder engine");
        return ret;
    }

    s_jpeg_buf_size = width * height * 2;
    jpeg_encode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };

    for (int i = 0; i < JPEG_BUFS; i++) {
        size_t allocated = 0;
        s_jpeg_buf[i] = (uint8_t *)jpeg_alloc_encoder_mem(s_jpeg_buf_size, &mem_cfg, &allocated);
        if (!s_jpeg_buf[i]) {
            ESP_LOGE(TAG, "Failed to allocate JPEG output buffer %d", i);
            return ESP_ERR_NO_MEM;
        }
        if (i == 0) s_jpeg_buf_size = allocated;
    }

    s_swap_mutex  = xSemaphoreCreateMutex();
    s_read_mutex  = xSemaphoreCreateMutex();
    s_frame_ready = xSemaphoreCreateBinary();

    ESP_LOGI(TAG, "JPEG HW encoder initialized: %dx%d, out_buf=%u bytes x2",
             width, height, (unsigned)s_jpeg_buf_size);
    return ESP_OK;
}

esp_err_t jpeg_enc_encode_frame(const uint8_t *yuv422_data, size_t yuv422_len) {
    if (!s_jpeg_handle || !yuv422_data) return ESP_ERR_INVALID_STATE;

    int widx = -1;
    xSemaphoreTake(s_swap_mutex, portMAX_DELAY);
    // Find a buffer that is NEITHER currently being transmitted NOR the newest ready frame
    for (int i = 0; i < JPEG_BUFS; i++) {
        int idx = (s_write_idx + i) % JPEG_BUFS;
        if (idx != s_active_read_idx && idx != s_read_idx) {
            widx = idx;
            break;
        }
    }
    // Fallback if somehow none are perfectly free (shouldn't happen with BUFS=3)
    if (widx == -1) {
        for (int i = 0; i < JPEG_BUFS; i++) {
           int idx = (s_write_idx + i) % JPEG_BUFS;
           if (idx != s_active_read_idx) { widx = idx; break; }
        }
    }
    
    if (widx != -1) {
        s_write_idx = (widx + 1) % JPEG_BUFS;
    }
    xSemaphoreGive(s_swap_mutex);

    if (widx == -1) {
        // Should not happen if JPEG_BUFS >= 2, but just in case
        return ESP_ERR_NO_MEM;
    }

    jpeg_encode_cfg_t enc_cfg = {
        .width         = s_width,
        .height        = s_height,
        .src_type      = JPEG_ENCODE_IN_FORMAT_YUV422,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = s_jpeg_quality,
    };

    uint32_t out_size = 0;
    esp_err_t ret = jpeg_encoder_process(s_jpeg_handle, &enc_cfg,
                                         yuv422_data, yuv422_len,
                                         s_jpeg_buf[widx], s_jpeg_buf_size, &out_size);
    if (ret != ESP_OK) return ret;

    s_jpeg_len[widx] = out_size;

    xSemaphoreTake(s_swap_mutex, portMAX_DELAY);
    s_read_idx  = widx;
    s_write_idx = (widx + 1) % JPEG_BUFS;
    xSemaphoreGive(s_swap_mutex);

    xSemaphoreGive(s_frame_ready);
    return ESP_OK;
}

esp_err_t jpeg_enc_get_frame(uint8_t **out_buf, size_t *out_len, uint32_t timeout_ms) {
    if (xSemaphoreTake(s_frame_ready, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    xSemaphoreTake(s_read_mutex, portMAX_DELAY);
    xSemaphoreTake(s_swap_mutex, portMAX_DELAY);
    int ridx = s_read_idx;
    xSemaphoreGive(s_swap_mutex);

    if (ridx < 0) {
        xSemaphoreGive(s_read_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    s_active_read_idx = ridx;
    *out_buf = s_jpeg_buf[ridx];
    *out_len = s_jpeg_len[ridx];
    return ESP_OK;
}

void jpeg_enc_release_frame(void) { 
    s_active_read_idx = -1;
    xSemaphoreGive(s_read_mutex); 
}

esp_err_t jpeg_enc_set_quality(int quality) {
    s_jpeg_quality = clamp_jpeg_quality(quality);
    ESP_LOGI(TAG, "JPEG quality set to %d", s_jpeg_quality);
    return ESP_OK;
}

int jpeg_enc_get_quality(void) {
    return s_jpeg_quality;
}
