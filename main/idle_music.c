#include "idle_music.h"

#include "dfplayer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

extern int  g_bg_music_volume;
extern bool g_bg_music_enabled;

static const char *TAG = "idle_music";

#define IDLE_THRESHOLD_MS  60000UL   // 1 minute of no-touch → start playing
#define BG_TRACK           99        // file 0099 on the SD card
/* Track 99 is a ~3 minute 7 second piece of background music. The
 * polling loop must wait at least the full track length before
 * issuing another play command, otherwise the music restarts every
 * few seconds and the user only ever hears the opening bars. 220 s
 * gives ~13 s of headroom on a 3:07 clip. */
#define CLIP_LEN_MS        220000UL  // 3 minutes 40 s — full track + margin
#define POLL_TICK_MS       300UL

static volatile uint32_t s_last_touch_ms = 0;
static volatile bool     s_bg_active     = false;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void idle_music_register_touch(void)
{
    s_last_touch_ms = now_ms();
}

void idle_music_suppress_seconds(int seconds)
{
    if (seconds <= 0) return;
    /* Push the "last touch" timer forward so the idle-music task sees
     * idle < threshold and stops/skips playing for the duration. */
    uint32_t suppress_ms = (uint32_t)seconds * 1000U;
    if (suppress_ms > IDLE_THRESHOLD_MS) suppress_ms = IDLE_THRESHOLD_MS;
    s_last_touch_ms = now_ms() - (IDLE_THRESHOLD_MS - suppress_ms);
    /* If music is currently playing, stop it now. */
    if (s_bg_active) {
        dfplayer_stop();
        s_bg_active = false;
    }
}

static void idle_music_task(void *arg)
{
    (void)arg;

    /* Seed the timer so we don't immediately count "since boot" as
     * idle — wait the full IDLE_THRESHOLD_MS from now. */
    s_last_touch_ms = now_ms();

    while (1) {
        uint32_t t = now_ms();
        uint32_t idle = t - s_last_touch_ms;
        bool should_play = g_bg_music_enabled && (idle >= IDLE_THRESHOLD_MS);

        if (should_play) {
            if (!s_bg_active) {
                s_bg_active = true;
                ESP_LOGI(TAG, "Idle %lu ms — starting track %d at vol %d",
                         (unsigned long)idle, BG_TRACK, g_bg_music_volume);
            }
            int started_vol = g_bg_music_volume;
            dfplayer_play_track_force_vol(BG_TRACK, (uint8_t)started_vol);

            /* Wait for the clip to play, polling for activity every
             * POLL_TICK_MS so a tap interrupts the music quickly
             * instead of waiting for the full clip to end. Also poll
             * g_bg_music_volume — when the user adjusts the slider
             * mid-track we re-apply the new value immediately instead
             * of waiting ~3 minutes for the next clip to start. Same
             * for nav-sound clips that overwrite the HW volume: we
             * snap it back to the bg level. */
            uint32_t waited = 0;
            uint32_t last_vol_check_ms = 0;
            int last_applied_vol = started_vol;
            while (waited < CLIP_LEN_MS) {
                vTaskDelay(pdMS_TO_TICKS(POLL_TICK_MS));
                waited += POLL_TICK_MS;
                if (!g_bg_music_enabled ||
                    (now_ms() - s_last_touch_ms) < IDLE_THRESHOLD_MS) {
                    dfplayer_stop();
                    s_bg_active = false;
                    ESP_LOGI(TAG, "Bg music stopped (touch or disabled)");
                    break;
                }
                /* Re-apply volume every ~1 s: cheap (UART write), and
                 * keeps the audio level matching the user's slider. */
                uint32_t tnow = now_ms();
                if ((tnow - last_vol_check_ms) >= 1000) {
                    last_vol_check_ms = tnow;
                    if (g_bg_music_volume != last_applied_vol) {
                        ESP_LOGI(TAG, "Bg vol changed %d -> %d (mid-track)",
                                 last_applied_vol, g_bg_music_volume);
                        dfplayer_set_volume((uint8_t)g_bg_music_volume);
                        last_applied_vol = g_bg_music_volume;
                    }
                }
            }
        } else {
            if (s_bg_active) {
                dfplayer_stop();
                s_bg_active = false;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

void idle_music_start(void)
{
    static bool started = false;
    if (started) return;
    started = true;
    if (xTaskCreate(idle_music_task, "idle_music", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create idle_music task");
        started = false;
    }
}
