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
bool dispenser_is_busy(void);
int dispenser_seconds_left(void);
int dispenser_waiting_slot(void);
uint8_t dispenser_get_missed_slots(void);
void dispenser_confirm_meds(void);
void dispenser_skip_meds(void);

/** Clear the 12-hour refire guard for a specific slot. Call this when
 *  the user edits a slot's HH:MM via the UI — otherwise a test fire
 *  earlier today would silently block the slot for 12 hours regardless
 *  of the new time. slot_idx valid range: 0..6, else no-op. */
void dispenser_reset_slot_refire_guard(int slot_idx);

/** สั่งจ่ายยาแบบ Manual ทันที (Non-blocking) */
void dispenser_manual_dispense(int med_idx, int qty);

/** Clear-all: run an eject-all return cycle on every module
 *  sequentially. Used at boot when every cartridge reports count=0
 *  to flush any pills that might have been left over from a previous
 *  session. Non-blocking — spawns its own worker task.
 *  Returns false if a clear-all is already in progress. */
bool dispenser_clear_all_start(void);

/** Live state of a clear-all run.  current_module ranges:
 *    -1      → not running
 *     0..5   → currently clearing module i
 *     6      → finished, popup should auto-dismiss
 *  Pills_total accumulates IR-counted pills across every module
 *  cleared in the run (for the completion message). */
bool dispenser_clear_all_active(void);
int  dispenser_clear_all_current_module(void);
int  dispenser_clear_all_pills_total(void);
/* Running pill count for the module currently being cleared. Resets to
 * 0 each time we move to the next module. UI uses this to show a live
 * "พบยา N เม็ด" line inside the clear-all popup so the operator sees
 * the IR counter ticking during each module's eject cycles. */
int  dispenser_clear_all_pills_current(void);

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
 *   'S' = scheduled auto-dispense (only entries that actually released
 *         a pill — see 2026-05-14 spec "เก็บแค่ยาที่จ่ายออกมา").
 *   'X' = unknown / other (defensive only — code paths don't write it)
 */
typedef struct {
    uint32_t  timestamp;   // unix seconds (0 if RTC not synced)
    int16_t   med_idx;     // 0..DISPENSER_MED_COUNT-1
    int16_t   from_count;
    int16_t   to_count;
    char      source;
} dispenser_audit_entry_t;

/** Append an event to the audit ring (256 entries, newest wins).
 *  Side effect: persists the ring to NVS so reboots don't lose history. */
void dispenser_audit_log(int med_idx, int from_count, int to_count, char source);

/** Restore audit ring contents from NVS — call once early in boot
 *  (before any task that uses /log or /audit.json). Safe to call when
 *  there's no prior data; just leaves the ring empty. */
void dispenser_audit_load_nvs(void);

/** Number of audit entries currently held (0..256). */
size_t dispenser_audit_count(void);

/** Copy up to max_entries newest-first audit entries into out_entries.
 *  Returns the number actually written. */
size_t dispenser_audit_get(dispenser_audit_entry_t *out_entries, size_t max_entries);

/** Build a human-readable comma-separated list of meal-slot names from
 *  the bitmask in netpie_shadow_t.med[i].slots. Output respects the
 *  Telegram language setting (TH/EN). Example: slots=0x05 →
 *  "ก่อนอาหารเช้า, ก่อนอาหารกลางวัน". Writes "-" if mask is empty. */
void dispenser_format_slots_to_names(uint8_t slots_mask, char *buf, size_t buf_len);

// Google Sheets Logger
void google_sheets_log(const char *event, const char *meds, const char *detail);

/** Send a Telegram notification with a fresh camera snapshot attached.
 *  Lazy-inits the camera on first call, falls through to plain text if
 *  the photo grab fails. Used for dispense-complete, return-pills,
 *  count-adjust, name-change events so the caregiver sees what's in the
 *  cartridge after each significant action. */
void dispenser_telegram_photo_msg(const char *msg);

#ifdef __cplusplus
}
#endif
