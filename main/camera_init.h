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
 * /photo handlers so the camera only powers up when actually needed —
 * keeps the I2C bus free for VL53 bootstrap during normal boot. */
esp_err_t camera_ensure_initialized(void);

/** True if camera_init() succeeded at least once. */
bool      camera_is_initialized(void);

/** Reset lazy-init state so the next camera_ensure_initialized() re-runs
 * camera_init() from scratch. Called by the capture task when MIPI-CSI
 * frames stop arriving for ≥10 s — recovers without a board reboot. */
void      camera_mark_uninitialized(void);

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
