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

// Play a specific track at forced volume
void dfplayer_play_track_force_vol(uint16_t num, uint8_t force_vol);

// Set player language mode (0: Thai, 1: English)
void dfplayer_set_language(int lang_is_eng);

// Stop currently playing track
void dfplayer_stop(void);

#ifdef __cplusplus
}
#endif
