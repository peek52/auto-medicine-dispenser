# Overnight Fix Report — 2026-05-11

Patches applied while monitoring serial overnight. Build verified clean
(exit 0). Firmware NOT flashed yet — board kept on previous v10 build
for the all-night stability watch. Flash in morning to test.

## Summary

- **Critical fixed:** 4 of 4
- **High fixed:** 4 of 4
- **Medium fixed:** 5 of 8 (3 deferred / verified false positive)
- **Low fixed:** 2 of 8 (rest were false positives or doc-only)
- **Total bugs touched:** ~15 real fixes across 11 files

## Fixes by file

### main/netpie_mqtt.c
- **A1 (Critical):** `netpie_get_shadow()` now returns a pointer to a
  double-buffered published snapshot. `parse_shadow()` and any other
  writer fills the inactive buffer, then atomically swaps the pointer.
  Readers always see a fully-formed shadow even mid-MQTT-update.
- **A2 (Medium):** `s_last_rx_ticks` declared `volatile uint32_t` for
  coherent reads from non-locked callers.
- Added `shadow_publish_locked()`; `shadow_unlock()` now publishes on
  every release so any code path that mutates s_shadow under lock
  publishes automatically.
- `shadow_cache_load_locked` now publishes after restoring from NVS.

### main/telegram_bot.c
- **B2 (Critical-hardened):** Photo write loop bails on `w<0` socket
  error and on 5 consecutive `w==0` zero-progress writes. Footer is
  still gated on `total_written == photo_len`, so half-sent multipart
  bodies were never possible — but the new bail makes the failure
  log clearer.
- **B3 (Medium):** `telegram_extract_arg1` now REJECTS over-long args
  (returns false + log) instead of silently truncating to buffer size.

### main/dispenser_scheduler.c
- **H1 (Low):** `s_last_triggered[6]` → `s_last_triggered[7]` so the
  HH:MM null terminator always fits.
- **H2 (High):** Per-slot fire tracking via `s_slot_last_fire[7]` with
  a 12-hour refire guard. Added a 5-min grace window with proper
  midnight wrap math (`delta` folded around the day) so a busy
  scheduler that wakes at 00:01 still fires the 00:00 slot. Old
  strict `diff != 0` check would silently miss any slot delayed past
  the exact minute.
- **A3 / scheduled servo (High, prior session):** scheduled dispense
  now bails on `pca9685_go_work` failure instead of waiting 7.5 s for
  IR while the servo never moved.
- **B3 dispense state mutex (prior session):** `dispenser_confirm_meds`
  / `dispenser_skip_meds` wrap state mutations in
  `s_dispense_state_mux`; logging/Telegram/Sheets calls moved outside
  the critical section.
- **H3 (Low):** Boot-time loud log when emergency stop is loaded as
  active from NVS — prevents support tickets where the device is
  silent because a previous panic left e-stop set.

### main/dfplayer.c
- **L1 (High):** UART write failure no longer permanently disables
  audio. After a fail streak, `dy_send_cmd` self-heals by calling
  `dfplayer_init()` (rate-limited to once every 5 s) so a transient
  glitch doesn't kill alerts for the rest of the session.
- **L2 (Medium):** `dfplayer_set_volume` no longer caches the new
  volume in `s_current_hw_vol` when the UART send failed — next call
  with the same value will retry instead of being skipped.
- Added `esp_timer.h` include for the new timer math.

### main/camera_init.c
- **E2 (Critical):** `camera_get_new_buffer()` (ISR) and
  `camera_trans_finished()` (ISR) now wrap the buffer-index
  selection / `ready_buf_idx` update in
  `portENTER_CRITICAL_ISR(&s_cam_buf_mux)`. The encoder task path
  (`camera_task`) wraps its read of `ready_buf_idx` and writes of
  `enc_active_idx` in `portENTER_CRITICAL` (non-ISR variant). Prior
  to this, the encoder could re-pick a buffer the ISR was about to
  fill, causing intermittent JPEG corruption.

### main/web_handlers_status.c
- **I1 (Critical):** Added `extract_form_value_n(body, body_len, ...)`
  with a manual bounded scan (no `strstr` past the buffer).
  `extract_form_value()` is kept as a thin wrapper so existing
  callers keep working. Pattern matching now requires the key to
  start at the body or right after `&` / `?`, preventing false
  matches inside escaped values.

### main/wifi_sta.c
- **D1 (Medium):** `wifi_schedule_backoff_reconnect()` checks
  `esp_timer_is_active()` before re-arming; cleared a stuck
  `s_reconnect_pending` when the timer wasn't actually active.
  Returns cleanly on `ESP_ERR_INVALID_STATE` instead of falling
  through to `esp_wifi_connect()`.
- **D2 (Low):** `nvs_get_wifi()` now caps reads at 63 bytes so the
  64-byte caller buffer always has room for the null terminator;
  also force-terminates on read failure so the caller never sees
  uninitialized garbage.

### main/display_clock.cpp
- **F1 (Medium):** Added `s_disp_mutex` (FreeRTOS mutex) around
  `fill_rect()` so the static `buf` / `last_color` cache can't race
  between clock_task and any UI task drawing concurrently. Cache
  was correctly used inside the lock; the issue was only the lack
  of mutual exclusion.
- **F2 (Low):** `display_clock_set_ip()` writes `s_ip` and
  `s_ip_dirty` inside `portENTER_CRITICAL(&s_ip_mux)` so the
  clock_task render can't see a half-written IP string.
- Mutex created in `display_clock_init()`.

### main/display_clock.h
- (already done in earlier session) `display_clock_show_ultra_safe`
  declaration removed alongside ultra_safe code.

### main/offline_sync.c
- **C1 (High → improved logging):** `offline_sync_save_queues_locked`
  now captures and logs each `nvs_set_*` return code so a partial
  commit failure is visible in the log instead of silently corrupting
  the offline queue. Caller contract documented.

## Audit findings that were FALSE POSITIVES

These were flagged by the audit but the code already handled them
correctly. No code change made; they are listed for reference.

- **B1 (telegram s_photo_inflight)** — counter check + increment is
  inside a single `portENTER_CRITICAL` and every error path
  decrements. No race exists.
- **A3 (netpie JSON unescape)** — escape handling exists at line 200
  of `json_get_str` (skips `\` and copies the next char). Loose but
  correct for the data we receive.
- **E1 (camera VL53 wait holds I2C)** — the wait uses a non-locking
  `vl53l0x_multi_is_bootstrapping()` flag check, never holds the
  bus, and yields via `vTaskDelay` between polls.
- **G1 (FT6336U boot timer uninitialized)** — initialized lazily on
  first call (line 246: `if (s_boot_start_ms == 0) s_boot_start_ms = now`).
- **M1 (PCF8574 backoff race)** — the time check + update are inside
  `taskENTER_CRITICAL(&s_pcf_mux)`. The `s_in_error_mode` early-out
  is racy but harmless (worst case, an extra probe through).

## Deferred (real bugs but design-change too invasive for tonight)

- **J1** `jpeg_enc_get_frame` / `release_frame` watchdog — needs
  a frame-sequence numbering scheme. Documented in code comments;
  fix in a follow-up that touches the streaming protocol.
- **J2** DMA2D timeout tuning — the 3000 ms timeout is symptom-
  treatment. Real fix needs PSRAM budget monitoring before encoding.
- **M2** PCA9685 servo angle racy between ramp and `/servo/set` — rare
  in practice (user button + simultaneous ramp), low impact, defer.
- **K1** `g_bg_music_enabled` non-atomic read pair in idle_music — low
  impact (music starts/stops briefly), defer.
- **N2** boot-count uint32_t overflow — only matters after ~4 billion
  boots, defer.
- **I2** `url_decode_inplace` malformed-escape pass-through — current
  behavior is safe, just inefficient.
- **O1** `ds3231_get_time_str` reads system time, not RTC — by design;
  documented as such.
- **P1** CLI stdin race with logger — would need a separate UART for
  CLI; defer.
- **P2** liveness canary semantics — documentation-only; keep as is.

## Build status

```
exit code: 0
Project build complete.
Warnings: only pre-existing strncpy / format-truncation /
          missing-field-initializers (NOT from any patch tonight)
```

`build/unified_cam.bin` is ready to flash. **Recommend the user flash
in morning** (during awake hours) so any unexpected boot behavior can
be caught + recovered immediately.

## Overnight monitor

`task bhzsxsgg7` armed, persistent, watching COM8 for resets/panics/
errors/dispense events. Raw log appends to `mon_overnight.txt`; a
filtered timestamped event line is emitted on each notable line. If
silent overnight = good (board on v10 was stable when armed).
