#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the DFPlayer Mini via UART1 using pins from config.h
void dfplayer_init(void);

// Set volume (0-30)
void dfplayer_set_volume(uint8_t vol);

// Play a specific track number (e.g. 1 for 0001.mp3)
void dfplayer_play_track(uint16_t num);

// Stop currently playing track
void dfplayer_stop(void);

#ifdef __cplusplus
}
#endif
