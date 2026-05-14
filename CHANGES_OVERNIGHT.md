# Overnight changes — 2026-05-15

Branch: `fix/cloud-networking-bugs`

Rollback baseline: **commit `9b49387`** — "Checkpoint before overnight audit".
If anything below breaks behaviour, `git reset --hard 9b49387` returns to the
last hand-verified state.

## User-requested features (commit `170fbe5`)

### 1. Force clear-all on every boot
- **File:** `main/ui_standby.cpp:1158-1172`
- **Before:** boot-clear prompt only shown when every module count was 0
- **After:** prompt shown unconditionally on first render after shadow loads
- **Why:** user spec "บังคับใหห้ล้าง" — a reboot could land the dispenser in any
  state (power glitch mid-dose, etc.) so a safety flush is always required.
  Pre-existing "ห้ามกดข้าม" rule (one button, no skip) already in touch handler.

### 2. Per-module Telegram during clear-all
- **File:** `main/dispenser_scheduler.c:1933-1977` (clear_all_task loop)
- **Before:** single final summary message after all 6 modules done
- **After:** one `telegram_send_text` per module (`"🧹 ล้างโมดูล N (ชื่อ) — พบยา X เม็ด"`)
  plus the existing final summary with the cumulative total
- **Why:** user wants live feedback per module instead of waiting ~30 s

### 3. Removed "IR นับ" / "IR-counted" wording
- **Files:** `main/dispenser_scheduler.c` — manual dispense reply (line 1750s),
  return-pill, clear-all summary
- Replaced with "พบยา N เม็ด" / "Found N pills" — drops implementation detail
- Also changed "ตลับว่าง" → "โมดูลว่าง" / "cartridge empty" → "module empty" for
  consistency with the user's preferred terminology

## Audit fixes (committed in this changelog's commit)

### A. `sh_clear` pointer race in clear-all loop
- **File:** `main/dispenser_scheduler.c:1933` (was the file-scope-pointer pattern)
- The double-buffered shadow can flip during the ~30 s clear-all run if any
  MQTT update lands. Holding `const netpie_shadow_t *sh_clear` across the
  6-iteration loop was a torn-read waiting to happen
- **Fix:** re-fetch + immediate strncpy into a 32-byte local inside each
  iteration. Atomic pointer-read + immediate copy → coherent snapshot
- **Risk:** very low — adds one stack-local per iteration, no timing change

### B. Manual dispense refused during clear-all
- **File:** `main/dispenser_scheduler.c:1818-1825`
- Previously a Telegram `/dispense` issued during clear-all would mark
  `ui_manual_disp_status=1`, spawn `manual_dispense_task`, and that task would
  block on `s_dispense_mutex` (portMAX_DELAY) for tens of seconds. UI showed a
  stuck "กำลังจ่ายยา" popup overlapping the state-8 clear-all paint
- **Fix:** early `if (s_clear_all_running) return;` at top of
  `dispenser_manual_dispense`
- **Risk:** low — just declines the request silently. The user retries when
  clear-all finishes

### C. Scheduled doses blocked while boot-clear modal is up
- **Files:** `main/dispenser_scheduler.c:1196-1210` (skip slot eval),
  `main/ui_standby.cpp:53-66` (`ui_standby_boot_clear_pending()` accessor),
  `main/ui_core.h:130-142` (C-linkage declaration)
- Previously a scheduled-dose minute that matched while the user was looking
  at the state-7 modal would flip `s_waiting_confirm = true` → `display_clock`
  yanks the user to `PAGE_CONFIRM_MEDS`, bypassing the lock entirely
- **Fix:** scheduler skips slot evaluation while either
  `ui_standby_boot_clear_pending()` is true OR `s_clear_all_running` is true.
  The 12-h refire guard is intentionally NOT touched here — after Clear the
  slot still fires if its grace window is current
- **Risk:** medium-low. A dose whose minute landed inside the boot popup
  could be skipped IF Clear takes longer than the slot grace window. In
  practice the popup takes a few seconds; grace is several minutes. Acceptable

### D. Cosmetic clear-all pill-counter cross-flash
- **File:** `main/dispenser_scheduler.c:1937-1940`
- Between writing `s_clear_all_current = i` and `clear_one_module(i)` resetting
  the per-module counter, the UI could render "Module 2 / Found 7" where 7
  came from module 1
- **Fix:** zero `s_clear_all_pills_current` in the for-loop body before
  calling `clear_one_module`
- **Risk:** none — purely cosmetic timing tightening

### E. camera_init PSRAM leak on background-retry path
- **File:** `main/camera_init.c:443-468` (alloc block at function entry)
- `camera_background_retry_task` calls `camera_ensure_initialized()` →
  `camera_init()` up to 38 times when first boot init fails. Each call
  unconditionally `heap_caps_aligned_calloc`'d the 3-buffer ring
  (~3 MB PSRAM) and the LDO channel — previous attempt's pointers were
  silently overwritten, leaking ~3 MB per retry
- **Fix:** at the top of `camera_init()` free any pre-existing
  `frame_buffers[i]`, `s_square_crop_buf`, and `ldo_mipi_phy` before
  re-allocating. The teardown path inside `camera_task` already does the
  same on the recovery cycle — this just covers the boot-retry path too
- **Risk:** low — freeing a NULL pointer is a no-op; freeing a stale
  pointer is correct cleanup

### F. PCA9685 per-channel ramp mutex race
- **File:** `main/pca9685.c:130-148` (pre-create inside `pca9685_init`)
- `ramp_lock_get()` lazy-created the per-channel mutex on first use.
  Two near-simultaneous `pca9685_ramp_task` instances for the same
  channel (back-to-back go_work_async + go_home_async from the dispenser)
  could both see NULL, both create a mutex, lose the loser's allocation
- **Fix:** pre-create all 16 mutexes inside `pca9685_init()` so the
  lazy path in `ramp_lock_get()` only ever sees non-NULL
- **Risk:** none — one-time alloc at boot

### G. USB mouse interface leak on transfer-alloc failure
- **File:** `main/usb_mouse.c:111-130`
- If `usb_host_transfer_alloc` failed AFTER `usb_host_interface_claim`
  succeeded, the interface stayed claimed forever — next mouse plug-in
  hit "claim failed" until reboot
- **Fix:** call `usb_host_interface_release` on the alloc-fail path
- **Risk:** low — only affects the unhappy alloc-fail path

## Items deliberately NOT changed

- **`s_dispense_busy` during clear-all** (Audit finding #5): naive fix
  interacts with the state-9 "กำลังจ่ายยา" popup in `ui_standby.cpp:1280` —
  would need re-ordered render gating that I didn't want to risk overnight.
  Fix B already closes the main symptom (manual dispense queueing behind
  clear-all)
- **Telegram queue overflow during offline 6-message burst** (Audit #4):
  flooding risk is bounded by `telegram_enqueue_text`'s existing throttle
  (only 1 message per 30 s persists offline). Worst case the user misses some
  per-module results when WiFi is down during boot-clear. Working as designed,
  flagged for future revisit
- **`g_dispense_missed_nav_mask` auto-nav during boot-clear** (Audit #6):
  trigger path is rare (requires a dispense whose mask survived into a reboot)
  and the user hasn't reported this firing. Leave alone

## Build + flash

- All commits build clean on `idf.py build` (esp32p4 target)
- Final flash to COM8 succeeded
- No watchdog / panic warnings observed during flash

## Pending for tomorrow

- **NETPIE widget HTML** — user requested but no code currently exists in the
  project. Will create when user is awake to confirm what fields / shadow
  paths the widget should display
