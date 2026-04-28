#pragma once

#include <stdbool.h>
#include <stddef.h>   // size_t
#include <stdint.h>

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

/** Emergency stop — block all new dispense triggers (manual + scheduled)
 *  until dispenser_emergency_clear() is called. In-flight servo motion
 *  finishes naturally; new ones won't start. Persists across reboots
 *  via NVS so a stuck system stays stopped after auto-restart. */
void dispenser_emergency_set(void);
void dispenser_emergency_clear(void);
bool dispenser_emergency_active(void);

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
