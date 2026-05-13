#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Background ambient music driver. When `g_bg_music_enabled` is true and
 * the user hasn't touched the screen for IDLE_THRESHOLD_MS, plays track
 * 99 in a loop at `g_bg_music_volume`. Stops as soon as a touch is
 * registered via `idle_music_register_touch()`. Track 99 is shared
 * between Thai and English UI languages. */

void idle_music_start(void);          // spawn the background task
void idle_music_register_touch(void); // call on every successful touch press

/* Suppress idle music for at least `seconds` from now. Use from code
 * paths that play long audio sequences (pre-alerts, dispense flow) so
 * the bg music task doesn't interrupt them after a few seconds. */
void idle_music_suppress_seconds(int seconds);

#ifdef __cplusplus
}
#endif
