#include "pill_sensor_status.h"
#include "config.h"
#include <string.h>

static pill_sensor_status_t s_sensors[PILL_SENSOR_COUNT];

void pill_sensor_status_init_defaults(void)
{
    for (int i = 0; i < PILL_SENSOR_COUNT; ++i) {
        s_sensors[i].address       = 0x29;  // VL53L0X default addr (ใช้ TCA9548A แยก channel)
        s_sensors[i].present       = false;
        s_sensors[i].valid         = false;
        s_sensors[i].raw_mm        = -1;
        s_sensors[i].filtered_mm   = -1;
        s_sensors[i].pill_count    = -1;
        s_sensors[i].is_empty      = false;
        s_sensors[i].is_full       = false;
        s_sensors[i].full_dist_mm  = VL53_FULL_DIST_MM;
        s_sensors[i].pill_height_mm = VL53_PILL_HEIGHT_MM;
        s_sensors[i].max_pills      = VL53_MAX_PILLS;
        s_sensors[i].count_offset   = 0;
    }
}

void pill_sensor_status_mark_present(int idx, bool present)
{
    if (idx < 0 || idx >= PILL_SENSOR_COUNT) return;
    s_sensors[idx].present = present;
    if (!present) {
        s_sensors[idx].valid       = false;
        s_sensors[idx].raw_mm      = -1;
        s_sensors[idx].filtered_mm = -1;
        s_sensors[idx].pill_count  = -1;
        s_sensors[idx].is_empty    = false;
        s_sensors[idx].is_full     = false;
    }
}

void pill_sensor_status_set_reading(int idx, int raw_mm, int filtered_mm, bool valid)
{
    if (idx < 0 || idx >= PILL_SENSOR_COUNT) return;
    s_sensors[idx].raw_mm      = raw_mm;
    s_sensors[idx].filtered_mm = filtered_mm;
    s_sensors[idx].valid       = valid;
    pill_sensor_status_recalc(idx);
}

void pill_sensor_status_set_config(int idx, int full_dist_mm, int pill_height_mm, int max_pills)
{
    if (idx < 0 || idx >= PILL_SENSOR_COUNT) return;
    if (full_dist_mm  > 0) s_sensors[idx].full_dist_mm   = full_dist_mm;
    if (pill_height_mm > 0) s_sensors[idx].pill_height_mm = pill_height_mm;
    if (max_pills     > 0) s_sensors[idx].max_pills       = max_pills;
    pill_sensor_status_recalc(idx);
}

void pill_sensor_status_set_offset(int idx, int count_offset)
{
    if (idx < 0 || idx >= PILL_SENSOR_COUNT) return;
    if (count_offset < -50) count_offset = -50;
    if (count_offset > 50) count_offset = 50;
    s_sensors[idx].count_offset = count_offset;
    pill_sensor_status_recalc(idx);
}

void pill_sensor_status_recalc(int idx)
{
    if (idx < 0 || idx >= PILL_SENSOR_COUNT) return;
    pill_sensor_status_t *s = &s_sensors[idx];

    if (!s->valid || s->filtered_mm < 0) {
        s->pill_count = -1;
        s->is_empty   = false;
        s->is_full    = false;
        return;
    }

    int dist = s->filtered_mm;
    int h    = s->pill_height_mm;
    int full = s->full_dist_mm;
    int maxp = s->max_pills;

    // จำนวนยาที่หายไป = (ระยะ - ระยะยาเต็ม) / ความสูง 1 เม็ด
    int removed = (dist - full + (h / 2)) / h;  // round
    if (removed < 0) removed = 0;

    int count = maxp - removed + s->count_offset;  // signed +/- fine-tune
    if (count < 0) count = 0;
    if (count > maxp) count = maxp;

    s->pill_count = count;
    s->is_empty   = (count == 0);
    s->is_full    = (count >= maxp);
}

const pill_sensor_status_t *pill_sensor_status_get_all(void)
{
    return s_sensors;
}

const pill_sensor_status_t *pill_sensor_status_get(int idx)
{
    if (idx < 0 || idx >= PILL_SENSOR_COUNT) return NULL;
    return &s_sensors[idx];
}
