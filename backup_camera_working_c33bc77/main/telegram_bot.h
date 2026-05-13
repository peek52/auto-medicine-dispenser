#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TELEGRAM_LANG_EN = 0,
    TELEGRAM_LANG_TH = 1,
} telegram_language_t;

/**
 * @brief Initialize telegram bot background thread for receiving messages (optional)
 * For just sending out logs, init is not strictly required.
 */
void telegram_init(void);

/**
 * @brief Fire-and-forget task to send a text message to the owner's Chat ID.
 * @param msg The zero-terminated string to send.
 */
void telegram_send_text(const char *msg);

/**
 * @brief Fire-and-forget task to send a photo with an optional caption to the owner's Chat ID.
 * @param photo_buf The JPEG encoded blob memory pointer. Takes ownership and auto-frees.
 * @param photo_len Size of the JPEG payload.
 * @param caption The string caption mapped within the multipart bounds.
 */
void telegram_send_photo_with_text(uint8_t *photo_buf, size_t photo_len, const char *caption);
void telegram_send_test_message(void);
void telegram_send_test_snapshot(void);

telegram_language_t telegram_get_language(void);
void telegram_set_language(telegram_language_t lang);

#ifdef __cplusplus
}
#endif
