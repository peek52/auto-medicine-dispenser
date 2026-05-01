#pragma once

#include <stdbool.h>
#include <stddef.h>   // size_t
#include <stdint.h>
#include "esp_err.h"

// ─────────────────────────────────────────────────────────────────────────────
//  dispenser_scheduler.h — Medicine dispenser timing logic
//  เทียบเวลา RTC vs NETPIE schedule → trigger PCA9685 servo จ่ายยา
// ─────────────────────────────────────────────────────────────────────────────

#ifdef __cplusplus
extern "C" {
#endif

/** เริ่ม FreeRTOS task ตรวจสอบเวลาและจ่ายยาอัตโนมัติ
 *  เรียกหลัง netpie_init() และ camera_init() */
void dispenser_scheduler_start(void);

/** คืนข้อความ next dose สำหรับแสดงบนจอ
 *  format: "M.Af  09:30" หรือ "No schedule"
 *  buf ต้องมีขนาดอย่างน้อย 24 bytes */
void dispenser_get_next_dose_str(char *buf, size_t buf_len);

bool dispenser_is_waiting(void);
bool dispenser_is_empty_warning(void);
int dispenser_seconds_left(void);
int dispenser_waiting_slot(void);
uint8_t dispenser_get_missed_slots(void);
void dispenser_confirm_meds(void);
void dispenser_skip_meds(void);

/** สั่งจ่ายยาแบบ Manual ทันที (Non-blocking) */
void dispenser_manual_dispense(int med_idx, int qty);

/** Per-module IR sensor presence flag (default: all true).
 *  When false, the dispense loop skips IR detection and trusts the
 *  servo cycle as the pill counter — useful when only some modules
 *  have an IR beam wired, or when the IR sensor is unreliable. The
 *  flag is persisted in NVS. */
bool dispenser_ir_present(int med_idx);
void dispenser_ir_set_present(int med_idx, bool present);

/** IR calibration sample — captured at high resolution during one
 *  full servo cycle so the operator can see WHEN the pill passed
 *  the beam. */
typedef struct {
    uint32_t time_ms;     // ms since cycle start
    uint8_t  raw_byte;    // full PCF8574 read
    uint8_t  bit_low;     // 1 if module's bit was LOW (pill blocking)
} ir_cal_sample_t;

/** Run a single calibration dispense cycle for med_idx and capture
 *  IR samples at ~5 ms resolution. Does not update shadow/count or
 *  send Telegram — pure diagnostic. Returns ESP_OK on success and
 *  fills *out_count with the number of samples written. */
esp_err_t dispenser_ir_calibrate(int med_idx,
                                 ir_cal_sample_t *samples,
                                 int max_samples,
                                 int *out_count);

/** Emergency stop — block all new dispense triggers (manual + scheduled)
 *  until dispenser_emergency_clear() is called. In-flight servo motion
 *  finishes naturally; new ones won't start. Persists across reboots
 *  via NVS so a stuck system stays stopped after auto-restart. */
void dispenser_emergency_set(void);
void dispenser_emergency_clear(void);
bool dispenser_emergency_active(void);

/** Quiet hours — suppress *scheduled* dispense between [start, end]
 *  (24-hour minutes-of-day). Manual + Telegram /dispense still works.
 *  start == end OR both == 0 → quiet hours disabled.
 *  Window can wrap midnight (e.g. start=22*60, end=6*60). */
void dispenser_set_quiet_hours(int start_min, int end_min);
void dispenser_get_quiet_hours(int *start_min, int *end_min);
bool dispenser_in_quiet_hours(int cur_h, int cur_m);

/* Audit log entry exposed through /audit.json. Source codes:
 *   'M' = manual via touch UI / web manual button
 *   'S' = scheduled auto-dispense
 *   'W' = web/edit screen stock change (user typed new count)
 *   'V' = VL53 sensor sync (cartridge filled/refilled physically)
 *   'X' = unknown / other
 */
typedef struct {
    uint32_t  timestamp;   // unix seconds (0 if RTC not synced)
    int16_t   med_idx;     // 0..DISPENSER_MED_COUNT-1
    int16_t   from_count;
    int16_t   to_count;
    char      source;
} dispenser_audit_entry_t;

/** Append an event to the audit ring (32 entries, newest wins). */
void dispenser_audit_log(int med_idx, int from_count, int to_count, char source);

/** Copy up to max_entries newest-first audit entries into out_entries.
 *  Returns the number actually written. */
size_t dispenser_audit_get(dispenser_audit_entry_t *out_entries, size_t max_entries);

/** บันทึกการปรับจำนวนยาจากหน้า setup และส่ง audit ไป Telegram แบบหน่วงรวมเหตุการณ์ */
void dispenser_audit_stock_adjust(int med_idx, int old_count, int new_count);

// Google Sheets Logger
void google_sheets_log(const char *event, const char *meds, const char *detail);

#ifdef __cplusplus
}
#endif
