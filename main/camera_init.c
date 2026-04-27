#include "camera_init.h"
#include "driver/isp.h"
#include "hal/color_types.h"

#include "esp_cache.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_private/esp_cache_private.h"
#include "driver/i2c_master.h"
#include "esp_sccb_intf.h"
#include "esp_sccb_i2c.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_encoder.h"
#include "config.h"
#include "i2c_manager.h"
#include "driver/ledc.h"
#include <string.h>

static const char *TAG = "camera_init";

#define CAM_RETURN_ON_ERR(expr, msg) do {         \
    esp_err_t __err = (expr);                     \
    if (__err != ESP_OK) {                        \
        ESP_LOGE(TAG, "%s: %s", msg,              \
                 esp_err_to_name(__err));         \
        return __err;                             \
    }                                             \
} while (0)

// Control variable for camera frame logging
static bool s_log_frames = false;

void camera_toggle_log(bool enable) {
    s_log_frames = enable;
    if (enable) {
        ESP_LOGI(TAG, "Camera frame logging ENABLED");
    } else {
        ESP_LOGI(TAG, "Camera frame logging DISABLED");
    }
}

static esp_cam_ctlr_handle_t cam_handle = NULL;
static isp_proc_handle_t isp_proc = NULL;
static esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
static esp_cam_sensor_device_t *s_cam_sensor = NULL;
static bool s_hmirror = false;
static bool s_vflip = false;
static int s_brightness = 0;
static int s_contrast = 0;
static int s_saturation = 0;
static uint8_t *s_square_crop_buf = NULL;
static size_t s_square_crop_buf_size = 0;
static size_t s_cache_line_size = 64;

static int camera_output_width(void)
{
    return CSI_HRES;
}

static int camera_output_height(void)
{
    return CSI_VRES;
}

static const uint8_t *camera_prepare_encode_frame(const uint8_t *src_buf, size_t src_len, size_t *out_len)
{
    const int out_w = camera_output_width();
    const int out_h = camera_output_height();

    if (out_len) *out_len = src_len;
    if (!src_buf) return NULL;
    if (CSI_HRES == out_w && CSI_VRES == out_h) return src_buf;
    if (!s_square_crop_buf || s_square_crop_buf_size < (size_t)(out_w * out_h * 2)) return src_buf;

    const int src_stride = CSI_HRES * 2;
    const int crop_stride = out_w * 2;
    const int crop_x = (CSI_HRES - out_w) / 2;
    const int crop_y = (CSI_VRES - out_h) / 2;

    for (int row = 0; row < out_h; ++row) {
        const uint8_t *src_row = src_buf + ((crop_y + row) * src_stride) + (crop_x * 2);
        uint8_t *dst_row = s_square_crop_buf + (row * crop_stride);
        memcpy(dst_row, src_row, crop_stride);
    }

    esp_cache_msync(s_square_crop_buf, s_square_crop_buf_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    if (out_len) *out_len = (size_t)(out_w * out_h * 2);
    return s_square_crop_buf;
}


#define NUM_BUFFERS 3
static void *frame_buffers[NUM_BUFFERS] = {NULL, NULL, NULL};
static size_t frame_buffer_size = 0;
static volatile int dma_buf_idx = 0;
static volatile int ready_buf_idx = -1;
static volatile int enc_active_idx = -1;

static SemaphoreHandle_t frame_ready_sem;

static bool IRAM_ATTR camera_get_new_buffer(esp_cam_ctlr_handle_t handle,
                                            esp_cam_ctlr_trans_t *trans,
                                            void *user_data) {
    int next = -1;
    for (int i = 1; i <= NUM_BUFFERS; i++) {
        int idx = (dma_buf_idx + i) % NUM_BUFFERS;
        if (idx != enc_active_idx && idx != ready_buf_idx) {
            next = idx;
            break;
        }
    }
    if (next == -1) {
        for (int i = 1; i <= NUM_BUFFERS; i++) {
            int idx = (dma_buf_idx + i) % NUM_BUFFERS;
            if (idx != enc_active_idx) {
                next = idx;
                break;
            }
        }
    }
    if (next == -1) next = (dma_buf_idx + 1) % NUM_BUFFERS;

    trans->buffer = frame_buffers[next];
    trans->buflen = frame_buffer_size;
    dma_buf_idx = next;
    return false;
}

static bool IRAM_ATTR camera_trans_finished(esp_cam_ctlr_handle_t handle,
                                            esp_cam_ctlr_trans_t *trans,
                                            void *user_data) {
    int finished_idx = -1;
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (frame_buffers[i] == trans->buffer) {
            finished_idx = i;
            break;
        }
    }
    if (finished_idx >= 0) {
        ready_buf_idx = finished_idx;
    }
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(frame_ready_sem, &xHigherPriorityTaskWoken);
    return xHigherPriorityTaskWoken == pdTRUE;
}

static void camera_task(void *arg) {
    int frame_num = 0;
    ESP_LOGI(TAG, "Camera capture task started (MJPEG mode)");

    esp_cam_ctlr_trans_t trans_data = {
        .buffer = frame_buffers[0],
        .buflen = frame_buffer_size,
    };
    dma_buf_idx = 0;

    esp_err_t ret = esp_cam_ctlr_receive(cam_handle, &trans_data, ESP_CAM_CTLR_MAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Initial receive failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        if (xSemaphoreTake(frame_ready_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "Frame wait timeout, retrying...");
            continue;
        }
        frame_num++;
        int encode_idx = ready_buf_idx;
        if (encode_idx < 0 || encode_idx >= NUM_BUFFERS) continue;

        enc_active_idx = encode_idx;
        esp_cache_msync(frame_buffers[encode_idx], frame_buffer_size,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        // Skip the JPEG encode if no /stream client is connected. The
        // encoder uses the JPEG hardware DMA + ~1MB output buffer, so
        // running it 50 times a second with no viewer just burns CPU
        // and heap fragmentation. Snapshot endpoint still encodes on
        // demand, so /capture stays responsive.
        if (!jpeg_enc_has_clients()) {
            enc_active_idx = -1;
            continue;
        }

        size_t encode_len = frame_buffer_size;
        const uint8_t *encode_buf = camera_prepare_encode_frame((const uint8_t *)frame_buffers[encode_idx],
                                                                frame_buffer_size,
                                                                &encode_len);
        esp_err_t err = jpeg_enc_encode_frame(encode_buf, encode_len);
        enc_active_idx = -1;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "JPEG Encoding failed: %s", esp_err_to_name(err));
        } else {
            if (s_log_frames) {
                ESP_LOGI(TAG, "Frame %d encoded OK (buf%d)", frame_num, ready_buf_idx);
            }
        }
    }
}

esp_err_t camera_init(void) {
    frame_ready_sem = xSemaphoreCreateBinary();

    frame_buffer_size = CSI_HRES * CSI_VRES * 2;
    esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &s_cache_line_size);
    if (s_cache_line_size < 64) s_cache_line_size = 64;
    frame_buffer_size = (frame_buffer_size + s_cache_line_size - 1) & ~(s_cache_line_size - 1);

    for (int i = 0; i < NUM_BUFFERS; i++) {
        frame_buffers[i] = heap_caps_aligned_calloc(
            s_cache_line_size, 1, frame_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!frame_buffers[i]) {
            ESP_LOGE(TAG, "Failed to allocate frame buffer %d", i);
            return ESP_ERR_NO_MEM;
        }
    }

    const int output_w = camera_output_width();
    const int output_h = camera_output_height();
    if (output_w != CSI_HRES || output_h != CSI_VRES) {
        s_square_crop_buf_size = (size_t)(output_w * output_h * 2);
        s_square_crop_buf_size = (s_square_crop_buf_size + s_cache_line_size - 1) & ~(s_cache_line_size - 1);
        s_square_crop_buf = (uint8_t *)heap_caps_aligned_calloc(
            s_cache_line_size, 1, s_square_crop_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_square_crop_buf) {
            ESP_LOGE(TAG, "Failed to allocate square crop buffer");
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "Initializing JPEG encoder for %dx%d", output_w, output_h);
    esp_err_t ret = jpeg_enc_init(output_w, output_h);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "JPEG init failed"); return ret; }

    esp_ldo_channel_config_t ldo_config = {
        .chan_id = CAM_LDO_CHAN_ID,
        .voltage_mv = CAM_LDO_VOLTAGE_MV,
    };
    CAM_RETURN_ON_ERR(esp_ldo_acquire_channel(&ldo_config, &ldo_mipi_phy),
                      "Failed to acquire camera LDO");

    //---------------I2C Init------------------//
    i2c_master_bus_handle_t i2c_bus_handle = i2c_manager_get_bus_handle();
    if (i2c_bus_handle == NULL) {
        ESP_LOGE(TAG, "Failed to get shared I2C bus handle");
        return ESP_FAIL;
    }

    //---------------SCCB Init------------------//
    esp_cam_sensor_config_t cam_config = {
        .sccb_handle = NULL,
        .reset_pin = -1,
        .pwdn_pin = -1,
        .xclk_pin = -1,
        .sensor_port = ESP_CAM_SENSOR_MIPI_CSI,
    };

    // ---- Auto-detect camera sensor ----

    esp_cam_sensor_device_t *cam = NULL;
    for (esp_cam_sensor_detect_fn_t *p = &__esp_cam_sensor_detect_fn_array_start; p < &__esp_cam_sensor_detect_fn_array_end; ++p) {
        sccb_i2c_config_t sccb_conf = {
            .scl_speed_hz = 50000,
            .device_address = p->sccb_addr,
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        };
        ret = sccb_new_i2c_io(i2c_bus_handle, &sccb_conf, &cam_config.sccb_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SCCB init failed at 0x%02X: %s",
                     p->sccb_addr, esp_err_to_name(ret));
            cam_config.sccb_handle = NULL;
            continue;
        }

        cam = (*(p->detect))(&cam_config);
        if (cam) {
            break;
        }
        if (cam_config.sccb_handle) {
            esp_err_t del_ret = esp_sccb_del_i2c_io(cam_config.sccb_handle);
            if (del_ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to release SCCB handle: %s",
                         esp_err_to_name(del_ret));
            }
            cam_config.sccb_handle = NULL;
        }
    }

    if (!cam) {
        ESP_LOGE(TAG, "failed to detect camera sensor");
        return ESP_FAIL;
    }
    s_cam_sensor = cam;

    esp_cam_sensor_format_array_t cam_fmt_array = {0};
    esp_cam_sensor_query_format(cam, &cam_fmt_array);
    const esp_cam_sensor_format_t *parray = cam_fmt_array.format_array;
    esp_cam_sensor_format_t *cam_cur_fmt = NULL;
    for (int i = 0; i < cam_fmt_array.count; i++) {
        if (!strcmp(parray[i].name, CSI_FORMAT_NAME)) {
            cam_cur_fmt = (esp_cam_sensor_format_t *) &parray[i];
            break;
        }
    }

    if (!cam_cur_fmt) {
        ESP_LOGE(TAG, "failed to find matching camera format %s", CSI_FORMAT_NAME);
        return ESP_FAIL;
    }

    esp_err_t ret_fmt = esp_cam_sensor_set_format(cam, cam_cur_fmt);
    if (ret_fmt != ESP_OK) {
        ESP_LOGE(TAG, "Format set fail");
    }

    int enable_flag = 1;
    esp_cam_sensor_ioctl(cam, ESP_CAM_SENSOR_IOC_S_STREAM, &enable_flag);

    esp_cam_ctlr_csi_config_t csi_config = {
        .ctlr_id                = 0,
        .h_res                  = CSI_HRES,
        .v_res                  = CSI_VRES,
        .lane_bit_rate_mbps     = 200, // Reverted to 200 (400 caused timeout)
        .input_data_color_type  = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_YUV422,
        .data_lane_num          = 2,
        .byte_swap_en           = false,
        .queue_items            = 2,   // Reverted to 2
    };
    CAM_RETURN_ON_ERR(esp_cam_new_csi_ctlr(&csi_config, &cam_handle),
                      "Failed to create CSI controller");

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans  = camera_get_new_buffer,
        .on_trans_finished = camera_trans_finished,
    };
    CAM_RETURN_ON_ERR(esp_cam_ctlr_register_event_callbacks(cam_handle, &cbs, NULL),
                      "Failed to register camera callbacks");
    CAM_RETURN_ON_ERR(esp_cam_ctlr_enable(cam_handle),
                      "Failed to enable camera controller");

    esp_isp_processor_cfg_t isp_config = {
        .clk_hz                = 80 * 1000 * 1000, // Reverted to 80MHz
        .input_data_source     = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type = ISP_COLOR_RAW8,
        .output_data_color_type= ISP_COLOR_YUV422,
        .has_line_start_packet = false,
        .has_line_end_packet   = false,
        .h_res                 = CSI_HRES,
        .v_res                 = CSI_VRES,
    };
    CAM_RETURN_ON_ERR(esp_isp_new_processor(&isp_config, &isp_proc),
                      "Failed to create ISP processor");
    CAM_RETURN_ON_ERR(esp_isp_enable(isp_proc),
                      "Failed to enable ISP processor");

    CAM_RETURN_ON_ERR(esp_cam_ctlr_start(cam_handle),
                      "Failed to start camera controller");
    if (xTaskCreatePinnedToCore(camera_task, "cam_task", 8192, NULL, 7, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create camera task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Camera + JPEG HW Encoder Initialized OK");
    return ESP_OK;
}

esp_cam_sensor_device_t *camera_get_sensor(void) {
    return s_cam_sensor;
}

const char *camera_get_sensor_name(void) {
    return s_cam_sensor ? esp_cam_sensor_get_name(s_cam_sensor) : "Unknown";
}

esp_err_t camera_set_hmirror(bool enable) {
    if (!s_cam_sensor) return ESP_ERR_INVALID_STATE;
    int value = enable ? 1 : 0;
    esp_err_t ret = esp_cam_sensor_set_para_value(
        s_cam_sensor, ESP_CAM_SENSOR_HMIRROR, &value, sizeof(value));
    if (ret == ESP_OK) {
        s_hmirror = enable;
        ESP_LOGI(TAG, "Camera mirror set to %d", value);
    }
    return ret;
}

esp_err_t camera_set_vflip(bool enable) {
    if (!s_cam_sensor) return ESP_ERR_INVALID_STATE;
    int value = enable ? 1 : 0;
    esp_err_t ret = esp_cam_sensor_set_para_value(
        s_cam_sensor, ESP_CAM_SENSOR_VFLIP, &value, sizeof(value));
    if (ret == ESP_OK) {
        s_vflip = enable;
        ESP_LOGI(TAG, "Camera vflip set to %d", value);
    }
    return ret;
}

bool camera_get_hmirror(void) {
    return s_hmirror;
}

bool camera_get_vflip(void) {
    return s_vflip;
}

static esp_err_t camera_set_int_param(uint32_t param_id, int value)
{
    if (!s_cam_sensor) return ESP_ERR_INVALID_STATE;
    return esp_cam_sensor_set_para_value(s_cam_sensor, param_id, &value, sizeof(value));
}

esp_err_t camera_set_brightness(int value)
{
    if (value < -3) value = -3;
    if (value > 3) value = 3;
    esp_err_t ret = camera_set_int_param(ESP_CAM_SENSOR_BRIGHTNESS, value);
    if (ret == ESP_OK) {
        s_brightness = value;
        ESP_LOGI(TAG, "Camera brightness set to %d", value);
    }
    return ret;
}

esp_err_t camera_set_contrast(int value)
{
    if (value < -3) value = -3;
    if (value > 3) value = 3;
    esp_err_t ret = camera_set_int_param(ESP_CAM_SENSOR_CONTRAST, value);
    if (ret == ESP_OK) {
        s_contrast = value;
        ESP_LOGI(TAG, "Camera contrast set to %d", value);
    }
    return ret;
}

esp_err_t camera_set_saturation(int value)
{
    if (value < -3) value = -3;
    if (value > 3) value = 3;
    esp_err_t ret = camera_set_int_param(ESP_CAM_SENSOR_SATURATION, value);
    if (ret == ESP_OK) {
        s_saturation = value;
        ESP_LOGI(TAG, "Camera saturation set to %d", value);
    }
    return ret;
}

int camera_get_brightness(void)
{
    return s_brightness;
}

int camera_get_contrast(void)
{
    return s_contrast;
}

int camera_get_saturation(void)
{
    return s_saturation;
}
