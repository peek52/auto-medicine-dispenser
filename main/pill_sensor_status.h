#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PILL_SENSOR_COUNT 6

// ─────────────────────────────────────────────────────────────────────────────
//  pill_sensor_status_t — สถานะและค่าที่อ่านได้จาก VL53L0X แต่ละตลับ
// ─────────────────────────────────────────────────────────────────────────────
typedef struct {
    uint8_t address;          // I2C address (ทุกตัว = 0x29 เมื่อใช้ TCA9548A)
    bool    present;          // พบ sensor บน bus หรือไม่
    bool    valid;            // ค่าที่อ่านได้ valid ไหม
    int     raw_mm;           // ระยะดิบจาก sensor (mm)
    int     filtered_mm;      // ระยะหลัง EMA filter (mm)
    int     pill_count;       // จำนวนยาที่เหลือ (คำนวณจากระยะ)
    bool    is_empty;         // ยาหมด (ระยะ >= empty threshold)
    bool    is_full;          // ยาเต็ม (ระยะ <= full threshold + 1 pill)
    // Per-channel config (adjustable via web)
    int     full_dist_mm;     // ระยะยาเต็ม (default: VL53_FULL_DIST_MM)
    int     pill_height_mm;   // ความสูง 1 เม็ด (default: VL53_PILL_HEIGHT_MM)
    int     max_pills;        // จำนวนยาสูงสุด (default: VL53_MAX_PILLS)
} pill_sensor_status_t;

void pill_sensor_status_init_defaults(void);
void pill_sensor_status_mark_present(int idx, bool present);
void pill_sensor_status_set_reading(int idx, int raw_mm, int filtered_mm, bool valid);
void pill_sensor_status_set_config(int idx, int full_dist_mm, int pill_height_mm, int max_pills);
const pill_sensor_status_t *pill_sensor_status_get_all(void);
const pill_sensor_status_t *pill_sensor_status_get(int idx);

// คำนวณ pill_count, is_empty, is_full จาก filtered_mm และ config
void pill_sensor_status_recalc(int idx);

#ifdef __cplusplus
}
#endif
