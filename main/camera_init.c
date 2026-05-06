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
#include "pill_sensor_status.h"  /* PILL_SENSOR_COUNT for VL53 XSHUT quiesce */
#include "driver/gpio.h"          /* gpio_set_level for XSHUT toggle */
#include "driver/ledc.h"
#include "vl53l0x_multi.h"
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

/* One-shot helper task spawned by the capture task when MIPI-CSI
 * frames stop arriving. Sleeps long enough for AVDD to fully drain,
 * then calls camera_ensure_initialized() which re-runs the entire
 * camera_init() flow (LDO acquire, SCCB detect, CSI ctlr setup,
 * spawn a NEW camera_task). Self-deletes after firing. */
void camera_auto_reinit_helper(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGW(TAG, "Auto re-init starting (no user trigger needed)");
    esp_err_t r = camera_ensure_initialized();
    ESP_LOGW(TAG, "Auto re-init -> %s", esp_err_to_name(r));
    vTaskDelete(NULL);
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

    /* Track consecutive frame timeouts so we can self-recover the
     * MIPI-CSI subsystem when it gets stuck in a non-streaming state.
     * Field test showed `Frame wait timeout` looping forever (200+ s)
     * even though SCCB stream-on returned OK — only a full re-init
     * cycle (AVDD power-cycle + SCCB re-init + CSI ctlr restart) frees
     * it up. Without this auto-recovery, the user has to power-cycle
     * the whole board to get a photo again. */
    int consec_timeouts = 0;
    uint32_t last_recovery_ms = 0;
    while (1) {
        if (xSemaphoreTake(frame_ready_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
            consec_timeouts++;
            ESP_LOGW(TAG, "Frame wait timeout, retrying... (%d/10)",
                     consec_timeouts);
            uint32_t now_ms = esp_log_timestamp();
            /* After 10 consecutive 1-second timeouts (~10 s of no
             * frames), and at least 60 s since the last recovery
             * attempt, fully tear down the camera + MIPI controller
             * and re-init from scratch. The 60 s cool-down stops us
             * from looping forever on a truly dead sensor. */
            if (consec_timeouts >= 10 && (now_ms - last_recovery_ms) > 60000) {
                ESP_LOGE(TAG, "Camera frames stuck for 10 s — "
                              "performing full re-init");
                last_recovery_ms = now_ms;
                consec_timeouts = 0;

                /* Stop & free the CSI controller. */
                if (cam_handle) {
                    (void)esp_cam_ctlr_stop(cam_handle);
                    (void)esp_cam_ctlr_disable(cam_handle);
                    (void)esp_cam_ctlr_del(cam_handle);
                    cam_handle = NULL;
                }

                /* AVDD power-cycle so the OV5647 resets cleanly. */
                if (ldo_mipi_phy) {
                    (void)esp_ldo_release_channel(ldo_mipi_phy);
                    ldo_mipi_phy = NULL;
                }
                vTaskDelay(pdMS_TO_TICKS(800));

                /* Mark camera uninitialized so a fresh init runs. */
                camera_mark_uninitialized();

                ESP_LOGW(TAG, "Capture task exiting — auto re-init "
                              "in 2 s");

                /* Auto-trigger re-init in a one-shot helper task so the
                 * user doesn't have to send /photo again to wake the
                 * camera. Spawn the helper BEFORE we delete ourselves
                 * so it's alive even after this task is gone. */
                extern void camera_auto_reinit_helper(void *arg);
                xTaskCreate(camera_auto_reinit_helper, "cam_reinit",
                            4096, NULL, 4, NULL);
                vTaskDelete(NULL);
                return;
            }
            continue;
        }
        consec_timeouts = 0;
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
    /* Power-cycle the camera AVDD rail explicitly: when the whole system
     * (board + camera + I2C peripherals) was powered up simultaneously
     * the inrush current dipped 3V3 just enough to latch OV5647 in a
     * bad state — it would NACK every SCCB read until manually power
     * cycled. By acquiring → releasing → reacquiring the LDO here, we
     * force a clean cold-boot of the sensor's analog supply, mimicking
     * the "board first, camera plugged in later" sequence that the
     * user reported as the only working path.
     *
     * Acquire (LDO ON for whatever transient state)
     *   → release (LDO OFF — camera fully powers down)
     *     → 200 ms settle (analog rail discharges through bypass caps)
     *       → re-acquire (LDO ON, camera cold-boots clean)
     *         → 50 ms (sensor PLL lock + register defaults load)
     */
    CAM_RETURN_ON_ERR(esp_ldo_acquire_channel(&ldo_config, &ldo_mipi_phy),
                      "Failed to acquire camera LDO");
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_err_t rel = esp_ldo_release_channel(ldo_mipi_phy);
    if (rel != ESP_OK) {
        ESP_LOGW(TAG, "LDO release failed (%s) — skipping power-cycle",
                 esp_err_to_name(rel));
    } else {
        ldo_mipi_phy = NULL;
        /* AVDD off-time bumped 200 → 800 ms. Field test showed 200 ms
         * sometimes wasn't enough for the OV5647 internal capacitors
         * to drain — chip stayed in its previous register state and
         * SCCB read of PID returned NACK on every retry. With 800 ms
         * off-time the chip resets cleanly and PID reads succeed. */
        vTaskDelay(pdMS_TO_TICKS(800));
        CAM_RETURN_ON_ERR(esp_ldo_acquire_channel(&ldo_config, &ldo_mipi_phy),
                          "Failed to re-acquire camera LDO after power cycle");
        /* Power-on stabilisation extended 50 → 150 ms so the sensor's
         * internal PLL has time to lock before XCLK + SCCB sequence. */
        vTaskDelay(pdMS_TO_TICKS(150));
        ESP_LOGI(TAG, "Camera AVDD power-cycled cleanly");
    }

    //---------------XCLK Generation via LEDC------------------//
    // OV5647 needs an external clock to drive its internal logic.
    // Without XCLK the sensor can NACK or drop register writes even
    // though PID reads still succeed from a static latch. Generate
    // a 24 MHz square wave on CAM_XCLK_PIN using LEDC. Settle before
    // talking SCCB so the sensor's clock domain is locked.
    {
        ledc_timer_config_t xclk_timer = {
            .speed_mode      = LEDC_LOW_SPEED_MODE,
            .timer_num       = LEDC_TIMER_0,
            .duty_resolution = LEDC_TIMER_1_BIT,
            .freq_hz         = CAM_XCLK_FREQ,
            .clk_cfg         = LEDC_AUTO_CLK,
        };
        esp_err_t lt = ledc_timer_config(&xclk_timer);
        if (lt != ESP_OK) {
            ESP_LOGW(TAG, "XCLK timer config failed: %s — sensor may misbehave",
                     esp_err_to_name(lt));
        } else {
            ledc_channel_config_t xclk_chan = {
                .speed_mode = LEDC_LOW_SPEED_MODE,
                .channel    = LEDC_CHANNEL_0,
                .timer_sel  = LEDC_TIMER_0,
                .intr_type  = LEDC_INTR_DISABLE,
                .gpio_num   = CAM_XCLK_PIN,
                .duty       = 1,    // 50% with 1-bit resolution
                .hpoint     = 0,
            };
            esp_err_t lc = ledc_channel_config(&xclk_chan);
            if (lc != ESP_OK) {
                ESP_LOGW(TAG, "XCLK channel config failed: %s",
                         esp_err_to_name(lc));
            } else {
                ESP_LOGI(TAG, "XCLK %d Hz generated on GPIO%d",
                         CAM_XCLK_FREQ, CAM_XCLK_PIN);
                /* Bumped 20 → 200 ms — OV5647 needs >150 ms for its
                 * internal PLL + power-on reset to fully settle after
                 * XCLK starts. Talking SCCB too soon leaves the sensor
                 * in a partial-init state where transmit-receive (PID)
                 * works but burst transmit (set_format) flakes out. */
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }
    }

    //---------------I2C Init------------------//
    // The shared I2C bus arrives here in ESP_ERR_INVALID_STATE — the
    // FT6336U fallback-mode poller and PCF8574 boot probes wedge the
    // IDF v5.3.x master state machine, so the read of OV5647's PID
    // succeeds (single short combined transaction) but the long
    // ~250-register write burst that follows fails on every transmit.
    // i2c_master_bus_reset alone doesn't fix it; the SCCB device
    // handle keeps the stale state. Full teardown + re-init of the
    // master bus, then refetch the handle, gives a clean state for
    // the SCCB device handle that sccb_new_i2c_io will allocate.
    {
        /* Quiesce all 6 VL53L0X chips before SCCB detect. Symptom seen
         * in field logs (2026-05-06): when Ch3 init fails on all 3
         * XSHUT-toggle attempts, the residual chip state on the shared
         * I2C bus interferes with OV5647 PID readback (every retry of
         * sccb_i2c_transmit_receive_reg_a16v8 fails). When Ch3 happens
         * to init OK on first try, camera detect also passes — pure
         * race. Driving every VL53 XSHUT LOW for 20 ms drops them off
         * the bus so SCCB has a clean field. We don't restore XSHUT
         * here; the existing vl53_poll_all retry-probe path will bring
         * the sensors back online within the next 5 s poll cycle (or
         * sooner via vl53l0x_request_refresh after dispense events). */
        static const gpio_num_t kXshut[PILL_SENSOR_COUNT] = {
            VL53L0X_XSHUT_M1, VL53L0X_XSHUT_M2, VL53L0X_XSHUT_M3,
            VL53L0X_XSHUT_M4, VL53L0X_XSHUT_M5, VL53L0X_XSHUT_M6,
        };
        for (int i = 0; i < PILL_SENSOR_COUNT; ++i) {
            if (kXshut[i] >= 0) gpio_set_level(kXshut[i], 0);
        }
        ESP_LOGI(TAG, "VL53 XSHUT lowered before SCCB (chips offline)");
        vTaskDelay(pdMS_TO_TICKS(20));

        esp_err_t r = i2c_manager_recover_bus();
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "i2c_manager_recover_bus before SCCB: %s",
                     esp_err_to_name(r));
        } else {
            ESP_LOGI(TAG, "Shared I2C bus recovered before SCCB init");
        }
        // Sensor PLL/clock-domain settle so the first SCCB write
        // doesn't race the XCLK lock window.
        vTaskDelay(pdMS_TO_TICKS(150));
    }
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

    // ---- Auto-detect + init in one retry block ----
    //
    // On IDF v5.3.3 + ESP32-P4 + the shared I2C bus, the very first
    // i2c_master_transmit() to the SCCB device returns INVALID_STATE
    // even on a freshly recovered bus, while the preceding PID READ
    // (i2c_master_transmit_receive) succeeds. The error flag is sticky
    // on the device handle, so just bus-resetting and retrying with the
    // SAME handle never recovers. Each retry tears down the SCCB handle,
    // recovers the I2C bus, then re-creates the handle and re-runs the
    // detect → set_format → S_STREAM sequence so the writes happen on a
    // brand-new device handle.
    esp_cam_sensor_device_t *cam = NULL;
    esp_err_t ret_fmt = ESP_FAIL;
    esp_err_t ret_strm = ESP_FAIL;
    int enable_flag = 1;
    esp_cam_sensor_format_t *cam_cur_fmt = NULL;

    /* Retry count bumped 8 → 16. Each SCCB attempt costs ~200 ms (ping
     * + recovery), so 16 worst-case = ~3.2 s vs old 1.6 s — small price
     * for a much higher chance of catching the OV5647 in a wakeable
     * state. Mid-cycle (attempt 8) we redo the AVDD power-cycle in case
     * the first one wasn't enough. */
    for (int attempt = 0; attempt < 16; ++attempt) {
        if (attempt == 8 && ldo_mipi_phy) {
            /* Halfway through retries with no success — kick the camera
             * with a second AVDD power-cycle. This catches the case
             * where the first power-on landed in a brown-out window. */
            ESP_LOGW(TAG, "Camera still not detected at attempt 8 — "
                          "doing a second AVDD power-cycle");
            (void)esp_ldo_release_channel(ldo_mipi_phy);
            ldo_mipi_phy = NULL;
            vTaskDelay(pdMS_TO_TICKS(800));
            (void)esp_ldo_acquire_channel(&ldo_config, &ldo_mipi_phy);
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        // 1) Tear down anything left from a prior attempt + recover bus.
        if (cam_config.sccb_handle) {
            (void)esp_sccb_del_i2c_io(cam_config.sccb_handle);
            cam_config.sccb_handle = NULL;
        }
        cam = NULL;
        cam_cur_fmt = NULL;
        if (attempt > 0) {
            (void)i2c_manager_recover_bus();
            i2c_bus_handle = i2c_manager_get_bus_handle();
            if (!i2c_bus_handle) {
                ESP_LOGE(TAG, "Lost I2C bus handle during camera retry");
                return ESP_FAIL;
            }
            vTaskDelay(pdMS_TO_TICKS(150));
        }

        // 2) Hold the shared I2C mutex across the whole SCCB sequence.
        bool got_lock = (g_i2c_mutex && xSemaphoreTake(g_i2c_mutex, pdMS_TO_TICKS(1500)) == pdTRUE);
        if (g_i2c_mutex && !got_lock) {
            ESP_LOGW(TAG, "Camera attempt %d: I2C mutex busy, skipping (would race other clients)",
                     attempt + 1);
            continue;
        }

        // 3) Detect.
        for (esp_cam_sensor_detect_fn_t *p = &__esp_cam_sensor_detect_fn_array_start;
             p < &__esp_cam_sensor_detect_fn_array_end; ++p) {
            // SCCB speed reduced 400→100 kHz: at 400 kHz the OV5647
            // burst register writes after PID detect returned
            // ESP_ERR_INVALID_STATE on this board; 100 kHz is the
            // standard SCCB rate and gives the IDF v5.3.3 i2c master
            // state machine more headroom between transactions.
            /* Dropped 100 → 50 kHz: at 100 kHz the OV5647 set_format burst
             * (>40 register writes back-to-back) trips the IDF v5.3 i2c_master
             * state machine into INVALID_STATE on this board. 50 kHz gives
             * the driver enough inter-transaction headroom that it stops
             * leaving the state in a wedged config. */
            sccb_i2c_config_t sccb_conf = {
                .scl_speed_hz   = 50000,
                .device_address = p->sccb_addr,
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            };
            esp_err_t sret = sccb_new_i2c_io(i2c_bus_handle, &sccb_conf, &cam_config.sccb_handle);
            if (sret != ESP_OK) {
                cam_config.sccb_handle = NULL;
                continue;
            }
            cam = (*(p->detect))(&cam_config);
            if (cam) break;
            (void)esp_sccb_del_i2c_io(cam_config.sccb_handle);
            cam_config.sccb_handle = NULL;
        }

        if (cam) {
            // 4) Pick format.
            esp_cam_sensor_format_array_t cam_fmt_array = {0};
            esp_cam_sensor_query_format(cam, &cam_fmt_array);
            for (int i = 0; i < cam_fmt_array.count; i++) {
                if (!strcmp(cam_fmt_array.format_array[i].name, CSI_FORMAT_NAME)) {
                    cam_cur_fmt = (esp_cam_sensor_format_t *)&cam_fmt_array.format_array[i];
                    break;
                }
            }

            (void)i2c_master_bus_wait_all_done(i2c_bus_handle, 500);
            // The PID detect path uses transmit-receive (combined-start)
            // which leaves the IDF v5.3.3 i2c master state machine in a
            // configuration that rejects the next pure transmit with
            // INVALID_STATE. i2c_master_bus_reset clears that latch
            // without invalidating the SCCB device handle.
            (void)i2c_master_bus_reset(i2c_bus_handle);
            vTaskDelay(pdMS_TO_TICKS(150));

            // 5) Configure + stream-on. Retry set_format up to 8 times.
            // Between retries we issue an OV5647 software reset (write
            // 0x82 to register 0x3008) which dumps the sensor's internal
            // state machine and lets the next SCCB burst start fresh.
            // This breaks the IDF v5.3 i2c_master INVALID_STATE loop
            // because the sensor itself stops getting confused by
            // half-applied burst writes.
            if (cam_cur_fmt) {
                for (int sf = 0; sf < 8; ++sf) {
                    ret_fmt = esp_cam_sensor_set_format(cam, cam_cur_fmt);
                    if (ret_fmt == ESP_OK) break;
                    ESP_LOGW(TAG, "set_format inner retry %d: %s",
                             sf + 1, esp_err_to_name(ret_fmt));
                    (void)i2c_master_bus_wait_all_done(i2c_bus_handle, 300);
                    (void)i2c_master_bus_reset(i2c_bus_handle);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    /* Soft-reset OV5647 via SCCB direct write — bypasses
                     * the sensor driver's set_format path. Address 0x36,
                     * register 0x3008 (SYSTEM CONTROL00), value 0x82. */
                    if (cam_config.sccb_handle) {
                        uint8_t reset_buf[3] = {0x30, 0x08, 0x82};
                        (void)esp_sccb_transmit_reg_a16v8(cam_config.sccb_handle,
                                                          0x3008, 0x82);
                        (void)reset_buf;
                        vTaskDelay(pdMS_TO_TICKS(20));
                    }
                }
                if (ret_fmt == ESP_OK) {
                    ret_strm = esp_cam_sensor_ioctl(cam, ESP_CAM_SENSOR_IOC_S_STREAM, &enable_flag);
                }
            }
        }

        if (got_lock) xSemaphoreGive(g_i2c_mutex);

        if (cam && cam_cur_fmt && ret_fmt == ESP_OK && ret_strm == ESP_OK) {
            ESP_LOGI(TAG, "Camera detect + format + stream-on OK (attempt %d)", attempt + 1);
            break;
        }
        ESP_LOGW(TAG, "Camera attempt %d failed: cam=%p fmt_match=%d set_fmt=%s strm=%s",
                 attempt + 1, (void *)cam, cam_cur_fmt != NULL,
                 esp_err_to_name(ret_fmt), esp_err_to_name(ret_strm));
        // Fall through to next iteration which tears down + recovers.
        ret_fmt = ESP_FAIL;
        ret_strm = ESP_FAIL;
    }

    /* Restore VL53 XSHUT all at once (no inter-chip delay). The
     * staggered version (400 ms × 6) interfered with the OV5647
     * MIPI-CSI lock window during the 2.4 s stagger period, causing
     * "Frame wait timeout" continuously. Quick simultaneous restore
     * (~tens of µs) gets the chips out of the critical window before
     * MIPI tries to lock its first frame. The brief 6-chip inrush is
     * within the 5 V 10 A PSU budget verified by the user. */
    {
        static const gpio_num_t kXshut[PILL_SENSOR_COUNT] = {
            VL53L0X_XSHUT_M1, VL53L0X_XSHUT_M2, VL53L0X_XSHUT_M3,
            VL53L0X_XSHUT_M4, VL53L0X_XSHUT_M5, VL53L0X_XSHUT_M6,
        };
        for (int i = 0; i < PILL_SENSOR_COUNT; ++i) {
            if (kXshut[i] >= 0) gpio_set_level(kXshut[i], 1);
        }
        ESP_LOGI(TAG, "VL53 XSHUT restored after SCCB");
    }

    if (!cam) {
        ESP_LOGE(TAG, "Failed to detect camera sensor after retries");
        return ESP_FAIL;
    }
    s_cam_sensor = cam;
    if (!cam_cur_fmt) {
        ESP_LOGE(TAG, "Failed to find matching camera format %s", CSI_FORMAT_NAME);
        return ESP_FAIL;
    }
    if (ret_fmt != ESP_OK) {
        ESP_LOGE(TAG, "Format set fail after retries — camera will not stream");
        return ret_fmt;
    }
    if (ret_strm != ESP_OK) {
        ESP_LOGE(TAG, "Sensor stream-on failed after retries");
        return ret_strm;
    }

    esp_cam_ctlr_csi_config_t csi_config = {
        .ctlr_id                = 0,
        .h_res                  = CSI_HRES,
        .v_res                  = CSI_VRES,
        /* MIPI lane bit rate dropped 200 → 150 Mbps to give the
         * controller more eye-time per bit. Field test showed
         * stream-on succeeded but the CSI host couldn't lock onto
         * frames at 200 Mbps even with healthy SCCB — lowering bit
         * rate is the standard remedy for marginal CSI signal
         * integrity. 150 Mbps × 2 lanes = 300 Mbps still gives ample
         * bandwidth for 800×640 RAW8 @ 5 fps (~26 Mbps actual). */
        .lane_bit_rate_mbps     = 150,
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

/* Lazy-init state — protected by a mutex so two concurrent /photo +
 * /capture calls don't both try to init the camera. */
static SemaphoreHandle_t s_lazy_init_mux = NULL;
static esp_err_t          s_lazy_init_result = ESP_FAIL;
static bool               s_lazy_init_done   = false;

bool camera_is_initialized(void)
{
    return s_lazy_init_done && s_lazy_init_result == ESP_OK;
}

/* Force the lazy-init machinery to re-run on the next photo / dispense
 * event. Called from the capture task when MIPI-CSI gets stuck and we
 * tear down the controller for a fresh start. Safe to call even if
 * the mutex hasn't been created yet (boot path) — the next
 * camera_ensure_initialized() call will see the cleared flag. */
void camera_mark_uninitialized(void)
{
    if (s_lazy_init_mux) {
        xSemaphoreTake(s_lazy_init_mux, portMAX_DELAY);
    }
    s_lazy_init_done = false;
    s_lazy_init_result = ESP_FAIL;
    if (s_lazy_init_mux) {
        xSemaphoreGive(s_lazy_init_mux);
    }
}

esp_err_t camera_ensure_initialized(void)
{
    /* Fast path: already done — return cached result without taking lock. */
    if (s_lazy_init_done) return s_lazy_init_result;

    if (!s_lazy_init_mux) {
        /* First-time setup — race here is unlikely (handlers run on the
         * httpd / telegram tasks which are serialised by their own queues),
         * but use static-init helper to be safe. */
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        if (!m) return ESP_ERR_NO_MEM;
        if (s_lazy_init_mux) {
            vSemaphoreDelete(m);
        } else {
            s_lazy_init_mux = m;
        }
    }

    xSemaphoreTake(s_lazy_init_mux, portMAX_DELAY);
    if (!s_lazy_init_done) {
        ESP_LOGI(TAG, "Lazy camera_init triggered (first /capture or /photo)");
        /* Quiesce the VL53 poll loop so the SCCB burst has the I2C bus
         * to itself. Otherwise concurrent VL53 retry probes contend with
         * the OV5647 PID read and the camera detect NACKs. Calls nest, so
         * we always resume even if camera_init returns early. */
        vl53l0x_multi_pause();
        /* Brief settle so any in-flight VL53 op finishes before SCCB. */
        vTaskDelay(pdMS_TO_TICKS(250));
        s_lazy_init_result = camera_init();
        vl53l0x_multi_resume();
        s_lazy_init_done   = true;
        ESP_LOGI(TAG, "Lazy camera_init -> %s",
                 esp_err_to_name(s_lazy_init_result));

        /* Warm-up: the CSI controller starts producing frames a few
         * hundred ms after stream-on. If the caller (Telegram /photo)
         * grabs the JPEG immediately, the buffer is still all-zeros from
         * heap_caps_aligned_calloc and the user gets a solid-black image.
         * Wait long enough for camera_task to fill at least 2 frames so
         * the buffer rotation lands on real pixel data. AE / AGC also
         * need this time to converge to a non-clipped exposure. */
        if (s_lazy_init_result == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(700));
        }
    }
    esp_err_t r = s_lazy_init_result;
    xSemaphoreGive(s_lazy_init_mux);
    return r;
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
