#include "cloud_secrets.h"
#include "config.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "cloud_secrets";
static const char *ACCESS_CODE_RESET_REV = "20260401b";
static const char *OWNER_OVERRIDE_CODE = "admin16";

static bool s_loaded = false;
static char s_tg_token[160] = {0};
static char s_tg_chat_id[32] = {0};
static char s_google_script_url[256] = {0};
static char s_cloud_access_code[32] = {0};
static char s_tech_access_code[32] = {0};
static char s_admin_access_code[32] = {0};

static void load_secret_str(nvs_handle_t h, const char *key, char *dst, size_t dst_len)
{
    size_t sz = dst_len;
    if (nvs_get_str(h, key, dst, &sz) == ESP_OK && dst[0] != '\0') {
        return;
    }

    dst[0] = '\0';
}

static void load_setting_with_default(nvs_handle_t h, const char *key, char *dst, size_t dst_len, const char *fallback)
{
    size_t sz = dst_len;
    if (nvs_get_str(h, key, dst, &sz) == ESP_OK && dst[0] != '\0') {
        return;
    }

    if (fallback && fallback[0] != '\0') {
        strncpy(dst, fallback, dst_len - 1);
        dst[dst_len - 1] = '\0';
        (void)nvs_set_str(h, key, dst);
    } else {
        dst[0] = '\0';
    }
}

void cloud_secrets_init(void)
{
    if (s_loaded) return;

    nvs_handle_t h;
    if (nvs_open("cloud_cfg", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open cloud_cfg NVS namespace");
        return;
    }

    load_secret_str(h, "tg_token", s_tg_token, sizeof(s_tg_token));
    load_secret_str(h, "tg_chat", s_tg_chat_id, sizeof(s_tg_chat_id));
    load_secret_str(h, "gs_url", s_google_script_url, sizeof(s_google_script_url));
    load_setting_with_default(h, "cloud_pin", s_cloud_access_code, sizeof(s_cloud_access_code), CLOUD_ACCESS_CODE_DEFAULT);
    load_setting_with_default(h, "tech_pin", s_tech_access_code, sizeof(s_tech_access_code), TECH_ACCESS_CODE_DEFAULT);
    load_setting_with_default(h, "admin_pin", s_admin_access_code, sizeof(s_admin_access_code), ADMIN_ACCESS_CODE_DEFAULT);
    {
        char rev[16] = {0};
        size_t rev_sz = sizeof(rev);
        bool rev_ok = (nvs_get_str(h, "code_rev", rev, &rev_sz) == ESP_OK);
        if (!rev_ok || strcmp(rev, ACCESS_CODE_RESET_REV) != 0) {
            snprintf(s_cloud_access_code, sizeof(s_cloud_access_code), "%s", CLOUD_ACCESS_CODE_DEFAULT);
            snprintf(s_tech_access_code, sizeof(s_tech_access_code), "%s", TECH_ACCESS_CODE_DEFAULT);
            snprintf(s_admin_access_code, sizeof(s_admin_access_code), "%s", ADMIN_ACCESS_CODE_DEFAULT);
            (void)nvs_set_str(h, "cloud_pin", s_cloud_access_code);
            (void)nvs_set_str(h, "tech_pin", s_tech_access_code);
            (void)nvs_set_str(h, "admin_pin", s_admin_access_code);
            (void)nvs_set_str(h, "code_rev", ACCESS_CODE_RESET_REV);
        }
    }
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "Cloud secrets loaded from NVS (telegram=%s, sheets=%s)",
             s_tg_token[0] ? "set" : "empty",
             s_google_script_url[0] ? "set" : "empty");
    s_loaded = true;
}

const char *cloud_secrets_get_telegram_token(void)
{
    cloud_secrets_init();
    return s_tg_token;
}

const char *cloud_secrets_get_telegram_chat_id(void)
{
    cloud_secrets_init();
    return s_tg_chat_id;
}

const char *cloud_secrets_get_google_script_url(void)
{
    cloud_secrets_init();
    return s_google_script_url;
}

bool cloud_secrets_has_telegram(void)
{
    cloud_secrets_init();
    return s_tg_token[0] != '\0' && s_tg_chat_id[0] != '\0';
}

bool cloud_secrets_has_google_script(void)
{
    cloud_secrets_init();
    return s_google_script_url[0] != '\0';
}

bool cloud_secrets_store(const char *tg_token, const char *tg_chat_id, const char *gs_url)
{
    nvs_handle_t h;
    if (nvs_open("cloud_cfg", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open cloud_cfg for write");
        return false;
    }

    const char *safe_token = tg_token ? tg_token : "";
    const char *safe_chat = tg_chat_id ? tg_chat_id : "";
    const char *safe_url = gs_url ? gs_url : "";

    if (nvs_set_str(h, "tg_token", safe_token) != ESP_OK ||
        nvs_set_str(h, "tg_chat", safe_chat) != ESP_OK ||
        nvs_set_str(h, "gs_url", safe_url) != ESP_OK ||
        nvs_commit(h) != ESP_OK) {
        nvs_close(h);
        ESP_LOGE(TAG, "Failed to store cloud secrets");
        return false;
    }
    nvs_close(h);

    strncpy(s_tg_token, safe_token, sizeof(s_tg_token) - 1);
    s_tg_token[sizeof(s_tg_token) - 1] = '\0';
    strncpy(s_tg_chat_id, safe_chat, sizeof(s_tg_chat_id) - 1);
    s_tg_chat_id[sizeof(s_tg_chat_id) - 1] = '\0';
    strncpy(s_google_script_url, safe_url, sizeof(s_google_script_url) - 1);
    s_google_script_url[sizeof(s_google_script_url) - 1] = '\0';
    s_loaded = true;

    ESP_LOGI(TAG, "Cloud secrets updated via runtime config");
    return true;
}

bool cloud_secrets_verify_owner_override_code(const char *code)
{
    return code && code[0] && strcmp(code, OWNER_OVERRIDE_CODE) == 0;
}

bool cloud_secrets_verify_cloud_access_code(const char *code)
{
    cloud_secrets_init();
    if (!code || !code[0] || !s_cloud_access_code[0]) return false;
    return strcmp(code, s_cloud_access_code) == 0;
}

bool cloud_secrets_verify_technician_access_code(const char *code)
{
    cloud_secrets_init();
    if (!code || !code[0] || !s_tech_access_code[0]) return false;
    return strcmp(code, s_tech_access_code) == 0;
}

bool cloud_secrets_verify_admin_access_code(const char *code)
{
    cloud_secrets_init();
    if (!code || !code[0] || !s_admin_access_code[0]) return false;
    return strcmp(code, s_admin_access_code) == 0;
}

bool cloud_secrets_store_access_codes(const char *cloud_code, const char *tech_code, const char *admin_code)
{
    cloud_secrets_init();

    char next_cloud[sizeof(s_cloud_access_code)] = {0};
    char next_tech[sizeof(s_tech_access_code)] = {0};
    char next_admin[sizeof(s_admin_access_code)] = {0};

    snprintf(next_cloud, sizeof(next_cloud), "%s",
             (cloud_code && cloud_code[0]) ? cloud_code : s_cloud_access_code);
    snprintf(next_tech, sizeof(next_tech), "%s",
             (tech_code && tech_code[0]) ? tech_code : s_tech_access_code);
    snprintf(next_admin, sizeof(next_admin), "%s",
             (admin_code && admin_code[0]) ? admin_code : s_admin_access_code);

    if (!next_cloud[0] || !next_tech[0] || !next_admin[0]) {
        ESP_LOGE(TAG, "Access codes must not be empty");
        return false;
    }

    nvs_handle_t h;
    if (nvs_open("cloud_cfg", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open cloud_cfg for access-code write");
        return false;
    }

    if (nvs_set_str(h, "cloud_pin", next_cloud) != ESP_OK ||
        nvs_set_str(h, "tech_pin", next_tech) != ESP_OK ||
        nvs_set_str(h, "admin_pin", next_admin) != ESP_OK ||
        nvs_commit(h) != ESP_OK) {
        nvs_close(h);
        ESP_LOGE(TAG, "Failed to store access codes");
        return false;
    }
    nvs_close(h);

    snprintf(s_cloud_access_code, sizeof(s_cloud_access_code), "%s", next_cloud);
    snprintf(s_tech_access_code, sizeof(s_tech_access_code), "%s", next_tech);
    snprintf(s_admin_access_code, sizeof(s_admin_access_code), "%s", next_admin);
    s_loaded = true;

    ESP_LOGI(TAG, "Access codes updated via runtime config");
    return true;
}
