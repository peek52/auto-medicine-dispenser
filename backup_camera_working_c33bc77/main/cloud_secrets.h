#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void cloud_secrets_init(void);

const char *cloud_secrets_get_telegram_token(void);
const char *cloud_secrets_get_telegram_chat_id(void);
const char *cloud_secrets_get_google_script_url(void);

bool cloud_secrets_has_telegram(void);
bool cloud_secrets_has_google_script(void);
bool cloud_secrets_store(const char *tg_token, const char *tg_chat_id, const char *gs_url);
bool cloud_secrets_verify_owner_override_code(const char *code);
bool cloud_secrets_verify_cloud_access_code(const char *code);
bool cloud_secrets_verify_technician_access_code(const char *code);
bool cloud_secrets_verify_admin_access_code(const char *code);
bool cloud_secrets_store_access_codes(const char *cloud_code, const char *tech_code, const char *admin_code);
const char *cloud_secrets_get_cloud_access_code(void);
const char *cloud_secrets_get_technician_access_code(void);
const char *cloud_secrets_get_admin_access_code(void);

#ifdef __cplusplus
}
#endif
