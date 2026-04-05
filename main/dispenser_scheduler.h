#pragma once

#include <stdbool.h>
#include <stddef.h>   // size_t

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
void dispenser_confirm_meds(void);
void dispenser_skip_meds(void);

/** สั่งจ่ายยาแบบ Manual ทันที (Non-blocking) */
void dispenser_manual_dispense(int med_idx, int qty);

/** บันทึกการปรับจำนวนยาจากหน้า setup และส่ง audit ไป Telegram แบบหน่วงรวมเหตุการณ์ */
void dispenser_audit_stock_adjust(int med_idx, int old_count, int new_count);

// Google Sheets Logger
void google_sheets_log(const char *event, const char *meds, const char *detail);

#ifdef __cplusplus
}
#endif
