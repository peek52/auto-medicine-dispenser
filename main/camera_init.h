#pragma once
#include "esp_err.h"
#include "esp_cam_sensor.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialise MIPI-CSI2 camera, ISP, JPEG HW encoder and start capture task */
esp_err_t camera_init(void);

/** Idempotent lazy-init wrapper. First call runs camera_init(); subsequent
 * calls are no-ops returning the cached result. Use from /capture and
 * /photo handlers so the camera only powers up when actually needed. */
esp_err_t camera_ensure_initialized(void);

/** True if camera_init() succeeded at least once. */
bool      camera_is_initialized(void);

/** Set TRUE while the SCCB (OV5647 over I2C0) burst sequence is running
 *  — typically ~3 s during init / recovery, when 40+ register writes go
 *  back-to-back. Other I2C consumers on the shared bus (touch poll,
 *  servo PWM) should check this and back off so they don't fight the
 *  camera for the mutex and force partial bursts. Volatile single-byte
 *  read is atomic on RISC-V. */
extern volatile bool g_camera_sccb_in_progress;

/** Reset lazy-init state so the next camera_ensure_initialized() re-runs
 * camera_init() from scratch. Called by the capture task when MIPI-CSI
 * frames stop arriving for ≥10 s — recovers without a board reboot. */
void      camera_mark_uninitialized(void);

/** Spawn a background task that retries camera_ensure_initialized()
 * every 8 s for up to ~5 minutes. Used at boot when the first
 * synchronous init attempt fails — keeps trying without blocking
 * the main task. Does nothing if a retry task is already running. */
void      camera_init_background_retry_start(void);

// เปิด/ปิดการแสดง log ของแต่ละเฟรมภาพ (เพื่อลดความรกบน monitor)
void camera_toggle_log(bool enable);
esp_cam_sensor_device_t *camera_get_sensor(void);
const char *camera_get_sensor_name(void);
esp_err_t camera_set_hmirror(bool enable);
esp_err_t camera_set_vflip(bool enable);
esp_err_t camera_set_brightness(int value);
esp_err_t camera_set_contrast(int value);
esp_err_t camera_set_saturation(int value);
bool camera_get_hmirror(void);
bool camera_get_vflip(void);
int camera_get_brightness(void);
int camera_get_contrast(void);
int camera_get_saturation(void);

#ifdef __cplusplus
}
#endif
