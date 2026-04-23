#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void offline_sync_init(void);
void offline_sync_queue_shadow_payload(const char *payload);
void offline_sync_queue_telegram_text(const char *msg);
void offline_sync_queue_google_sheets(const char *event, const char *meds, const char *detail);
void offline_sync_flush_async(void);
bool offline_sync_has_pending_work(void);
size_t offline_sync_pending_count(void);
size_t offline_sync_pending_shadow_count(void);
size_t offline_sync_pending_event_count(void);
size_t offline_sync_pending_telegram_count(void);
size_t offline_sync_pending_gsheet_count(void);

#ifdef __cplusplus
}
#endif
