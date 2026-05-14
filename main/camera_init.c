#include "camera_init.h"
#include "driver/isp.h"
#include "driver/isp_ccm.h"
#include "hal/color_types.h"
#include "soc/isp_reg.h"
#include "esp_rom_sys.h"

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
#include "driver/gpio.h"          /* gpio_set_level for XSHUT toggle */
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
// Reverted 2026-05-11: an earlier overnight patch (E2) tried to wrap
// the three indices above in portENTER_CRITICAL_ISR / portENTER_CRITICAL.
// Live-tested afterwards and Telegram /photo started returning all-black
// JPEGs — the synchronization disturbed the timing between DMA-finished
// ISR + encoder buffer pickup just enough to corrupt the captured frame.
// Going back to volatile-only access; the theoretical race the audit
// described has not manifested in months of field use.

static SemaphoreHandle_t frame_ready_sem;

/* Public flag — see camera_init.h. Set TRUE around the SCCB attempt loop
 * so the touch driver (ft6336u_read_touch) and other casual I2C clients
 * skip their reads instead of racing the camera for g_i2c_mutex. */
volatile bool g_camera_sccb_in_progress = false;

/* Lazy-init state — defined further down the file (near
 * camera_ensure_initialized). Forward-declare here so camera_task's
 * recovery path can synchronise teardown against a concurrent
 * ensure_init call. */
static SemaphoreHandle_t s_lazy_init_mux;
static bool              s_lazy_init_done   = false;
static esp_err_t         s_lazy_init_result = ESP_FAIL;

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

/* Background retry task spawned by main.c when the very first
 * camera_ensure_initialized() at boot returns FAIL. Keeps trying every
 * 8 s for up to 5 minutes (38 attempts). Most boards recover within
 * 1-3 tries — the OV5647 sometimes comes up in a stuck state on
 * cold boot but responds normally after one extra software reset
 * cycle. Self-deletes once camera is up or the budget is exhausted.
 * Only one instance runs at a time (handle gate). */
static TaskHandle_t s_cam_retry_handle = NULL;

static void camera_background_retry_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 38; ++i) {
        vTaskDelay(pdMS_TO_TICKS(8000));
        if (camera_is_initialized()) break;
        ESP_LOGW(TAG, "Background camera retry %d/38", i + 1);
        camera_mark_uninitialized();
        esp_err_t r = camera_ensure_initialized();
        ESP_LOGW(TAG, "Background camera retry %d -> %s",
                 i + 1, esp_err_to_name(r));
        if (r == ESP_OK) break;
    }
    s_cam_retry_handle = NULL;
    vTaskDelete(NULL);
}

void camera_init_background_retry_start(void)
{
    if (s_cam_retry_handle != NULL) return;
    if (xTaskCreate(camera_background_retry_task, "cam_retry", 4096,
                    NULL, 3, &s_cam_retry_handle) != pdPASS) {
        s_cam_retry_handle = NULL;
        ESP_LOGE(TAG, "Failed to create cam_retry task");
    }
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
            /* Cooldown: 15 s between recovery attempts. Auto-reinit
             * MUST also wait until the system has fully settled — log
             * 2026-05-12 showed 16 SCCB failures in a row when the
             * recovery fired DURING the WiFi + NETPIE TLS handshake
             * storm (~12-23 s into boot). The shared I2C bus was being
             * pulled left and right by other tasks during that window,
             * starving SCCB. By 60 s into boot the bus is quiet and the
             * camera comes back on attempt 1.
             *
             * Gates: (a) g_system_ready must be true (deferred_init has
             * finished firing all peripherals); (b) uptime > 30 s so
             * MQTT TLS handshake is also done; (c) regular 15 s cool-
             * down between attempts. */
            extern bool g_system_ready;
            bool sys_ready = g_system_ready && (now_ms > 30000);
            bool cd_ok = sys_ready &&
                         ((last_recovery_ms == 0) ||
                          ((now_ms - last_recovery_ms) > 15000));
            /* Reset the counter when we're forced to wait — otherwise
             * the log fills with "timeout 47/10, 48/10..." while the
             * system is busy with boot tasks. Each new "10-second
             * window" of waiting is a fresh attempt at the recovery
             * gate. */
            if (consec_timeouts >= 10 && !sys_ready) {
                consec_timeouts = 0;
                ESP_LOGI(TAG, "Camera auto-reinit deferred until system settled "
                              "(uptime=%lu ms, sys_ready=%d)",
                         (unsigned long)now_ms, (int)g_system_ready);
            }
            if (consec_timeouts >= 10 && cd_ok) {
                ESP_LOGE(TAG, "Camera frames stuck for 10 s — "
                              "performing full re-init");
                last_recovery_ms = now_ms;
                consec_timeouts = 0;

                /* Take the lazy-init mutex BEFORE tearing anything down.
                 * Without this, a concurrent camera_ensure_initialized()
                 * call (Telegram /photo, web /capture) can see the cached
                 * "init OK" flag right up to the moment we wipe it, then
                 * return ESP_OK to its caller while we're freeing the
                 * controller out from under it — caller gets a black /
                 * crashed photo. Marking uninit + tearing down all inside
                 * the mutex makes the slow-path ensure-init in another
                 * task block here until reinit completes. */
                if (s_lazy_init_mux) {
                    xSemaphoreTake(s_lazy_init_mux, portMAX_DELAY);
                }
                s_lazy_init_done   = false;
                s_lazy_init_result = ESP_FAIL;

                /* Free the binary semaphore — camera_init() guards
                 * recreate so it's safe to leave NULL here. Without this
                 * delete, each recovery cycle leaked one semaphore
                 * object plus its scheduler bookkeeping. */
                if (frame_ready_sem) {
                    vSemaphoreDelete(frame_ready_sem);
                    frame_ready_sem = NULL;
                }

                /* Stop & free the CSI controller. */
                if (cam_handle) {
                    (void)esp_cam_ctlr_stop(cam_handle);
                    (void)esp_cam_ctlr_disable(cam_handle);
                    (void)esp_cam_ctlr_del(cam_handle);
                    cam_handle = NULL;
                }

                /* Free the ISP processor — ESP32-P4 only has ONE ISP
                 * slot. Without this, the next esp_isp_new_processor()
                 * call returns ESP_ERR_NOT_FOUND and the whole re-init
                 * fails permanently until reboot. (This was the actual
                 * root cause of "auto-recovery doesn't bring camera
                 * back" reported by the user.) */
                if (isp_proc) {
                    (void)esp_isp_disable(isp_proc);
                    (void)esp_isp_del_processor(isp_proc);
                    isp_proc = NULL;
                }

                /* Drop the camera sensor handle so the next init's
                 * esp_cam_sensor_detect probes fresh. */
                s_cam_sensor = NULL;

                /* Free the DMA frame buffers (3 × 1 MB in PSRAM).
                 * Otherwise each recovery cycle leaks ~3 MB and after
                 * a few rounds the heap drops below the threshold
                 * for re-allocating during init. */
                for (int i = 0; i < NUM_BUFFERS; ++i) {
                    if (frame_buffers[i]) {
                        heap_caps_free(frame_buffers[i]);
                        frame_buffers[i] = NULL;
                    }
                }
                frame_buffer_size = 0;

                /* AVDD power-cycle so the OV5647 resets cleanly. */
                if (ldo_mipi_phy) {
                    (void)esp_ldo_release_channel(ldo_mipi_phy);
                    ldo_mipi_phy = NULL;
                }
                vTaskDelay(pdMS_TO_TICKS(800));

                /* Teardown done — release the mutex so the helper task
                 * we spawn below can take it via camera_ensure_initialized. */
                if (s_lazy_init_mux) {
                    xSemaphoreGive(s_lazy_init_mux);
                }

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
        /* Liveness: print the very first frame (confirmation) and then a
         * heartbeat every ~10 s (500 frames at 50fps) so the operator can
         * see in /logs/tail that MIPI is healthy without spamming the log
         * 50 times per second. Detailed per-frame logging is still
         * available via the `logcam on` CLI command. */
        if (frame_num == 1) {
            ESP_LOGI(TAG, "MIPI first frame received — pipeline healthy");
        } else if ((frame_num % 500) == 0) {
            ESP_LOGI(TAG, "MIPI alive: %d frames received", frame_num);
        }
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
    /* Guard against re-create on recovery path — the teardown block in
     * camera_task deletes this and sets it NULL, so a fresh init creates
     * one. But on the very first boot path nothing has run, so create
     * here too. */
    if (!frame_ready_sem) {
        frame_ready_sem = xSemaphoreCreateBinary();
    }

    frame_buffer_size = CSI_HRES * CSI_VRES * 2;
    esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &s_cache_line_size);
    if (s_cache_line_size < 64) s_cache_line_size = 64;
    frame_buffer_size = (frame_buffer_size + s_cache_line_size - 1) & ~(s_cache_line_size - 1);

    /* Free any buffers from a prior failed init before re-allocating.
     * Boot-time background retry (camera_background_retry_task) calls
     * camera_init() up to 38 times via camera_ensure_initialized(); if
     * a previous attempt failed AFTER alloc but BEFORE the camera_task
     * teardown path runs, frame_buffers[] / s_square_crop_buf still
     * point at live PSRAM. Without this free each retry leaks ~3 MB. */
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (frame_buffers[i]) {
            heap_caps_free(frame_buffers[i]);
            frame_buffers[i] = NULL;
        }
    }
    if (s_square_crop_buf) {
        heap_caps_free(s_square_crop_buf);
        s_square_crop_buf = NULL;
        s_square_crop_buf_size = 0;
    }
    if (ldo_mipi_phy) {
        (void)esp_ldo_release_channel(ldo_mipi_phy);
        ldo_mipi_phy = NULL;
    }

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
    /* MIPI PHY LDO acquire with brief power-cycle. ESP32-P4 LDO chan 3
     * powers the on-chip MIPI DPHY (NOT the OV5647 AVDD). A short
     * cycle (acquire → release → 200 ms → acquire) ensures the DPHY
     * registers come up at default state even if a previous boot left
     * them in an indeterminate config. Long off-times (>500 ms) caused
     * SCCB transmit_receive to return INVALID_STATE on subsequent
     * boots — likely because the DPHY's internal state machine got
     * stuck. 200 ms is the sweet spot: long enough for the rail to
     * fully drain, short enough not to disturb sensor-side comms. */
    CAM_RETURN_ON_ERR(esp_ldo_acquire_channel(&ldo_config, &ldo_mipi_phy),
                      "Failed to acquire MIPI PHY LDO");
    vTaskDelay(pdMS_TO_TICKS(20));
    if (esp_ldo_release_channel(ldo_mipi_phy) == ESP_OK) {
        ldo_mipi_phy = NULL;
        vTaskDelay(pdMS_TO_TICKS(200));
        CAM_RETURN_ON_ERR(esp_ldo_acquire_channel(&ldo_config, &ldo_mipi_phy),
                          "Failed to re-acquire MIPI PHY LDO");
        vTaskDelay(pdMS_TO_TICKS(80));
        ESP_LOGI(TAG, "MIPI PHY LDO power-cycled (200 ms off)");
    } else {
        ESP_LOGW(TAG, "MIPI PHY LDO release failed — skipping cycle");
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
    // The shared I2C bus can arrive here in ESP_ERR_INVALID_STATE if a
    // prior FT6336U poll wedged the IDF v5.3.x master state machine —
    // the short OV5647 PID read succeeds but the ~250-register write
    // burst that follows fails on every transmit. i2c_master_bus_reset
    // alone doesn't fix it; the SCCB device handle keeps the stale
    // state. Full teardown + re-init of the master bus gives a clean
    // state for the SCCB device handle that sccb_new_i2c_io allocates.
    {
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

    /* I²C bus scan — list every address 0x08-0x77 that ACKs so we know
     * what's actually on the bus right now. Helpful when SCCB read of
     * camera fails: shows whether the OV5647 isn't there at all (no
     * 0x36 ACK) or it moved to a different address (e.g. 0x21 / 0x3C
     * on some clones) or the bus itself is dead (nothing ACKs). Runs
     * once per camera_init() call. */
    {
        ESP_LOGW(TAG, "I2C scan begin (looking for camera + others)...");
        char hits_line[80];
        size_t hits_n = 0;
        hits_line[0] = '\0';
        for (uint8_t a = 0x08; a <= 0x77; ++a) {
            if (i2c_manager_ping(a) == ESP_OK) {
                hits_n += snprintf(hits_line + hits_n,
                                   sizeof(hits_line) - hits_n,
                                   "0x%02X ", a);
                if (hits_n >= sizeof(hits_line) - 6) {
                    ESP_LOGW(TAG, "I2C scan ACK: %s", hits_line);
                    hits_n = 0;
                    hits_line[0] = '\0';
                }
            }
        }
        if (hits_n > 0) {
            ESP_LOGW(TAG, "I2C scan ACK: %s", hits_line);
        }
        ESP_LOGW(TAG, "I2C scan end. Camera should be at 0x36; "
                      "PCA=0x40 RTC=0x68 Touch=0x38");
    }

    /* Signal touch + other shared-bus clients to back off — see
     * camera_init.h. Without this the FT6336U poll (every 10-15 ms from
     * clock_task) wins the i2c_manager mutex between camera attempts,
     * inserting touch reads in the middle of the SCCB burst and tripping
     * the IDF i2c_master state machine into NACK. Was the visible cause
     * of "Frame wait timeout" loops on warm restart. */
    g_camera_sccb_in_progress = true;

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
            /* Before destroying the handle, try a sensor-level soft reset
             * via SCCB direct write. Catches the case where the previous
             * attempt's set_format burst left OV5647 in a half-configured
             * state that makes subsequent PID readback fail. Register
             * 0x0103 = 0x01 = "software reset", per OV5647 datasheet —
             * loads default register values. We retry blindly here
             * (best effort, no error check) since the handle is about
             * to be torn down anyway. */
            (void)esp_sccb_transmit_reg_a16v8(cam_config.sccb_handle, 0x0103, 0x01);
            vTaskDelay(pdMS_TO_TICKS(20));
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
                /* Don't leak the SCCB-busy flag — touch driver checks
                 * this on every poll and would skip all reads forever
                 * (visible as "screen suddenly unresponsive" with no
                 * obvious log line). Was the cause of the user's
                 * "อยู่ๆ ก็สัมผัสไม่ได้" report 2026-05-15. */
                g_camera_sccb_in_progress = false;
                return ESP_FAIL;
            }
            /* Longer settle after retry — sensor needs ≥10 ms to come
             * out of soft reset, then a few hundred more to be fully
             * ready for the next register burst. */
            vTaskDelay(pdMS_TO_TICKS(250));
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

            // 5) Configure ONLY (no stream-on yet). S_STREAM 1 must be
            // issued AFTER the CSI controller has its DMA armed and the
            // bridge enabled — otherwise the sensor pumps MIPI bits into
            // a not-yet-listening PHY and the very first frame is missed,
            // sometimes leaving the DPHY out of sync forever ("Frame wait
            // timeout" symptom). Retry set_format up to 8 times with a
            // soft sensor reset between failures so a corrupted SCCB
            // burst can recover.
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
                    /* Verify a critical register actually applied. PCLK
                     * period byte at 0x4837 should be 40 (= 1e9 / 25e6).
                     * If SCCB byte loss corrupted the format burst, this
                     * register often reads back as 0xFF, 0x00, or some
                     * other stale value. Force a retry so we don't stream
                     * with a half-configured sensor. */
                    if (cam_config.sccb_handle) {
                        uint8_t pclk_period = 0;
                        esp_err_t rb = esp_sccb_transmit_receive_reg_a16v8(
                            cam_config.sccb_handle, 0x4837, &pclk_period);
                        ESP_LOGW(TAG, "set_format readback 0x4837=0x%02X (%s, expect 0x28)",
                                 pclk_period, esp_err_to_name(rb));
                        if (rb != ESP_OK || pclk_period != 0x28) {
                            ESP_LOGW(TAG, "PCLK reg verify failed — forcing attempt retry");
                            ret_fmt = ESP_FAIL;
                        }
                    }
                    /* S_STREAM (sensor pumps MIPI data) is INTENTIONALLY
                     * NOT called here — see "step 8" near the end of this
                     * function. ret_strm is set to ESP_OK only to keep
                     * the outer success-check happy; the actual stream
                     * start happens after CSI is ready. */
                    if (ret_fmt == ESP_OK) ret_strm = ESP_OK;
                }
            }
        }

        if (got_lock) xSemaphoreGive(g_i2c_mutex);

        if (cam && cam_cur_fmt && ret_fmt == ESP_OK && ret_strm == ESP_OK) {
            ESP_LOGI(TAG, "Camera detect + format OK (attempt %d) — stream-on deferred until CSI ready",
                     attempt + 1);
            break;
        }
        ESP_LOGW(TAG, "Camera attempt %d failed: cam=%p fmt_match=%d set_fmt=%s",
                 attempt + 1, (void *)cam, cam_cur_fmt != NULL,
                 esp_err_to_name(ret_fmt));
        // Fall through to next iteration which tears down + recovers.
        ret_fmt = ESP_FAIL;
        ret_strm = ESP_FAIL;
    }

    /* SCCB burst done — let touch + others resume normal I2C access. */
    g_camera_sccb_in_progress = false;

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
        .lane_bit_rate_mbps     = 400,
        .input_data_color_type  = CAM_CTLR_COLOR_RAW8,
        /* Switched RGB565 — the YUV422 path on this OV5647 + ESP32-P4
         * combo produced grayscale output (chroma was getting zeroed
         * somewhere in the rgb2yuv conversion). RGB565 is what the IDF
         * camera_dsi reference example uses successfully and gives
         * proper colour preservation through demosaic. JPEG encoder
         * accepts RGB565 directly. */
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,
        .data_lane_num          = 2,
        .byte_swap_en           = false,
        .queue_items            = 2,
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

    /* Sensor 0x4800 will be set to 0x10 (LINE_SYNC=1, BUS_IDLE=0) by
     * our override after S_STREAM. That keeps the line-start short
     * packets the IDF camera_dsi reference flow expects, but disables
     * the BUS_IDLE bit so the MIPI clock lane stays in HS between
     * frames instead of dropping to LP-11. The HS→LP→HS transition
     * was where this board's DPHY was losing lock after the very first
     * frame. ISP must keep has_line_start_packet=true to match. */
    esp_isp_processor_cfg_t isp_config = {
        .clk_hz                = 80 * 1000 * 1000,
        .input_data_source     = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type = ISP_COLOR_RAW8,
        /* RGB565 output — see CSI controller comment above. */
        .output_data_color_type= ISP_COLOR_RGB565,
        .has_line_start_packet = true,
        .has_line_end_packet   = true,
        .h_res                 = CSI_HRES,
        .v_res                 = CSI_VRES,
    };
    CAM_RETURN_ON_ERR(esp_isp_new_processor(&isp_config, &isp_proc),
                      "Failed to create ISP processor");
    CAM_RETURN_ON_ERR(esp_isp_enable(isp_proc),
                      "Failed to enable ISP processor");

    /* Override ISP bayer_mode register — the demosaic block in
     * ESP32-P4 ISP defaults to mode 0 (BG/GR = BGGR) which should
     * theoretically match the OV5647's BGGR output, but field testing
     * showed a strong purple cast in the produced YUV422 stream
     * (purple = excess R+B, missing G). That symptom is classic
     * R/B-channel swap in demosaic, so we override bayer_mode to 11
     * (RG/GB = RGGB) which puts the demosaic in the opposite phase.
     * If the resulting image still has wrong colors, try the other
     * two modes (01 = GBRG, 10 = GRBG) in turn.
     *
     * Register: ISP_FRAME_CFG_REG at DR_REG_ISP_BASE + 0x10
     * Field:    bayer_mode at bits 27-28 (2 bits) */
    {
        /* DEBUG: bayer_mode left at the value the IDF driver wrote
         * (default 0 = BGGR). Both 0 and 3 (RGGB) tested previously
         * produced similar purple cast → bayer alignment isn't the
         * primary issue. Logging the current register value so we
         * can see what's set. */
        volatile uint32_t *reg = (volatile uint32_t *)ISP_FRAME_CFG_REG;
        uint32_t v = *reg;
        unsigned cur = (unsigned)((v >> 27) & 0x3U);
        ESP_LOGI(TAG, "ISP frame_cfg = 0x%08lx, bayer_mode = %u (00=BGGR 01=GBRG 10=GRBG 11=RGGB)",
                 (unsigned long)v, cur);
    }

    /* CCM disabled — diagonal 2.2× pushed pixel values into saturation
     * which created the very dark output with vertical banding (looks
     * like JPEG compression on near-black data). Brightness boost will
     * be applied at sensor side instead, via OV5647 SDE Y-offset
     * register written after S_STREAM. */

    CAM_RETURN_ON_ERR(esp_cam_ctlr_start(cam_handle),
                      "Failed to start camera controller");

    /* Step 8 — NOW that the CSI controller is started (DMA armed,
     * bridge enabled, callback ready to provide buffers), tell the
     * sensor to leave LP-11 and pump real MIPI bits. This is the
     * single most important ordering fix: doing S_STREAM before
     * esp_cam_ctlr_start() leaves the receiver missing the very first
     * SoF on a 50 fps stream and the DPHY can lose lock permanently
     * until full re-init. With this order the receiver is ready
     * BEFORE the first sensor packet, matching the IDF camera_dsi
     * reference flow. */
    {
        int enable_strm = 1;
        esp_err_t r = esp_cam_sensor_ioctl(s_cam_sensor,
                                           ESP_CAM_SENSOR_IOC_S_STREAM,
                                           &enable_strm);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "Sensor S_STREAM 1 (post-CSI-start) failed: %s",
                     esp_err_to_name(r));
            return r;
        }

        /* OV5647 driver's S_STREAM writes 0x4800 = 0x14 (BUS_IDLE bit 2
         * set + LINE_SYNC bit 4 set) when CONFIG_CAMERA_OV5647_CSI_
         * LINESYNC_ENABLE=y. With BUS_IDLE the MIPI clock lane goes to
         * LP-11 between frames; on this board the DPHY can't reliably
         * relock at the start of frame 2, producing the "MIPI first
         * frame received → Frame wait timeout" symptom. Override to
         * 0x10 (LINE_SYNC bit 4 only, BUS_IDLE cleared) so the clock
         * lane stays in HS between frames while line short packets
         * still flow. ISP keeps has_line_start_packet=true to match. */
        if (cam_config.sccb_handle) {
            esp_err_t ovr = esp_sccb_transmit_reg_a16v8(
                cam_config.sccb_handle, 0x4800, 0x10);
            ESP_LOGI(TAG, "Override 0x4800=0x10 (continuous clock + line sync): %s",
                     esp_err_to_name(ovr));

            /* OV5647 SDE Y-offset only — adds a positive bias to the
             * luma channel without touching U/V. The sensor's internal
             * ISP first applies SDE before output, so this brightens
             * the YUV stream from the start. No saturation or hue
             * manipulation = no risk of colour cast.
             *
             * Register 0x5580 SDE_CTRL0:
             *   bit 7 = Y-offset enable    ← ONLY THIS BIT
             *   bit 6 = Y-gamma enable
             *   bit 5 = V-offset enable
             *   bit 4 = U-offset enable
             *   bit 3 = contrast enable
             *   bit 1 = saturation enable
             *
             * Register 0x5588 = sign byte (bit 0 = Y sign: 0=positive)
             * Register 0x5587 = Y-offset magnitude (0..255) */
            (void)esp_sccb_transmit_reg_a16v8(cam_config.sccb_handle, 0x5580, 0x80);
            (void)esp_sccb_transmit_reg_a16v8(cam_config.sccb_handle, 0x5588, 0x00);
            (void)esp_sccb_transmit_reg_a16v8(cam_config.sccb_handle, 0x5587, 0x40);
            ESP_LOGI(TAG, "OV5647 SDE Y-offset = +64 (brightness boost, no chroma)");
        }
        ESP_LOGI(TAG, "Sensor stream-on issued AFTER CSI ready — first frame should arrive shortly");
    }

    if (xTaskCreatePinnedToCore(camera_task, "cam_task", 8192, NULL, 7, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create camera task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Camera + JPEG HW Encoder Initialized OK");
    return ESP_OK;
}

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
    /* Fast path: only cache SUCCESS. If a previous attempt failed,
     * retry on every call so the user can recover by sending another
     * /photo (or by physically reseating the cable + sending /photo).
     * Earlier behaviour cached the failure permanently — once camera
     * init failed once, every subsequent /photo returned the cached
     * error without retrying, so the user got "Camera failed to
     * initialise" forever even after the underlying issue was fixed. */
    if (s_lazy_init_done && s_lazy_init_result == ESP_OK) {
        return ESP_OK;
    }
    /* Otherwise fall through and re-run camera_init. The mutex
     * below serialises concurrent attempts. */
    s_lazy_init_done = false;

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
        vTaskDelay(pdMS_TO_TICKS(150));
        s_lazy_init_result = camera_init();
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
