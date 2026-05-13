#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * usb_mouse.h — USB HID Mouse support for ESP32-P4 via USB OTG Host
 *
 * Call usb_mouse_start() once at startup.
 * Poll usb_mouse_get() from the display task to read cursor position.
 */

typedef struct {
    int16_t  x;          // cursor X (clamped to screen bounds)
    int16_t  y;          // cursor Y (clamped to screen bounds)
    bool     btn_left;   // left button currently held
    bool     btn_right;  // right button
    bool     changed;    // position or button changed since last read
} mouse_state_t;

/**
 * Start the USB Host + HID Mouse task.
 * Must be called AFTER usb_phy_init() / normal app_main USB setup.
 */
void usb_mouse_start(void);

/**
 * Get a snapshot of the current mouse state.
 * Resets changed = false after reading.
 */
mouse_state_t usb_mouse_get(void);

/** Returns true if a USB mouse is currently connected */
bool usb_mouse_is_connected(void);

/** Screen bounds used for clamping (set before starting) */
extern int16_t usb_mouse_screen_w;
extern int16_t usb_mouse_screen_h;

#ifdef __cplusplus
}
#endif
