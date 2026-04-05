#include "wifi_sta.h"
#include "config.h"
#include "esp_log.h"
#ifndef CONFIG_WIFI_RMT_CACHE_TX_BUFFER_NUM
#define CONFIG_WIFI_RMT_CACHE_TX_BUFFER_NUM 32
#endif
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "offline_sync.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdbool.h>

static const char *TAG = "wifi_sta";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_SCAN_DONE_BIT  BIT2
#define MAX_RETRY           10

static EventGroupHandle_t s_wifi_event_group;
static char s_ip_str[16] = "0.0.0.0";
static char s_ssid_str[33] = "";
static bool s_connected  = false;
static int  s_retry_num  = 0;

static esp_err_t start_sta(const char *ssid, const char *pass);
static void start_ap(void);

/* ── NVS helpers ── */
void nvs_get_wifi(char *ssid, char *pass)
{
    nvs_handle_t h;
    if (nvs_open("wifi_cfg", NVS_READONLY, &h) == ESP_OK) {
        size_t sz = 64;
        nvs_get_str(h, "ssid", ssid, &sz);
        sz = 64;
        nvs_get_str(h, "pass", pass, &sz);
        nvs_close(h);
    }
}

esp_err_t wifi_sta_save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("wifi_cfg", NVS_READWRITE, &h));
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "WiFi credentials saved to NVS");
    return ESP_OK;
}

esp_err_t wifi_sta_reconnect(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "Dynamically applying new WiFi: %s", ssid);
    wifi_sta_save_credentials(ssid, pass);
    strncpy(s_ssid_str, ssid ? ssid : "", sizeof(s_ssid_str) - 1);
    s_ssid_str[sizeof(s_ssid_str) - 1] = '\0';

    s_retry_num = 0;
    esp_wifi_disconnect();

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid,     ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    // Switch to STA mode completely (disables setup AP without a reboot)
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_connect();

    return ESP_OK;
}

extern void display_clock_set_ip(const char *ip);

void wifi_sta_forget(void) {
    ESP_LOGI(TAG, "Forgetting WiFi credentials...");
    wifi_sta_save_credentials("", "");
    s_retry_num = MAX_RETRY; // Block re-attempts
    esp_wifi_disconnect();
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    strncpy(s_ip_str, "0.0.0.0", sizeof(s_ip_str));
    s_ssid_str[0] = '\0';
    s_connected = false;
    display_clock_set_ip("0.0.0.0");
}

/* ── Event handler ── */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
        }
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry %d/%d...", s_retry_num, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            display_clock_set_ip("0.0.0.0"); // Update UI on fail
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        s_retry_num = 0;
        ESP_LOGI(TAG, "Connected — IP: %s", s_ip_str);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        display_clock_set_ip(s_ip_str); // Update UI on connect
        offline_sync_flush_async();
    }
}

/* ── STA init ── */
static esp_err_t start_sta(const char *ssid, const char *pass)
{
    s_retry_num = 0;
    strncpy(s_ssid_str, ssid ? ssid : "", sizeof(s_ssid_str) - 1);
    s_ssid_str[sizeof(s_ssid_str) - 1] = '\0';
    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h1, h2;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, &h1));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, &h2));

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid,     ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT) return ESP_OK;
    return ESP_FAIL;
}

/* ── AP fallback ── */
static void start_ap(void)
{
    ESP_LOGI(TAG, "Starting AP mode: unified_cam_setup");
    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid            = "unified_cam_setup",
            .ssid_len        = 0,
            .password        = "",
            .max_connection  = 4,
            .authmode        = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA)); // APSTA mode allows both hosting AP and scanning
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    snprintf(s_ip_str, sizeof(s_ip_str), "192.168.4.1");
    ESP_LOGI(TAG, "AP started — connect to 'unified_cam_setup' then open http://192.168.4.1/wifi");
}

esp_err_t wifi_sta_init(void)
{
    // Try NVS credentials first, then fall back to defaults
    char ssid[64] = WIFI_SSID_DEFAULT;
    char pass[64] = WIFI_PASS_DEFAULT;
    nvs_get_wifi(ssid, pass);  // overwrite with NVS if available

    esp_err_t ret = start_sta(ssid, pass);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "STA connect failed — fallback to AP mode");
        start_ap();
    }
    return ESP_OK;
}

const char *wifi_sta_get_ip(void)  { return s_ip_str; }
const char *wifi_sta_get_ssid(void) { return s_ssid_str; }
bool        wifi_sta_connected(void) { return s_connected; }

#define MAX_SCANNED_APS 15
static wifi_ap_record_t s_scan_results[MAX_SCANNED_APS];
static uint16_t s_scan_count = 0;
static volatile bool s_scan_in_progress = false;

static void async_scan_worker_task(void *arg)
{
    wifi_scan_config_t scan_config = {
        .ssid        = 0,
        .bssid       = 0,
        .channel     = 0,
        .show_hidden = false
    };
    
    s_scan_in_progress = true;
    
    ESP_LOGI(TAG, "Worker Task: Executing WiFi scan...");
    
    // Use block=true because we are already in a hidden background task
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Worker Task: esp_wifi_scan_start failed: %d", err);
        s_scan_count = 0;
        s_scan_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > MAX_SCANNED_APS) ap_count = MAX_SCANNED_APS;
    
    if (ap_count > 0) {
        esp_wifi_scan_get_ap_records(&ap_count, s_scan_results);
    }

    s_scan_count = ap_count;
    s_scan_in_progress = false;

    ESP_LOGI(TAG, "Worker Task: Scan complete! Found %d networks", ap_count);
    vTaskDelete(NULL);
}

esp_err_t wifi_sta_start_scan(void)
{
    if (s_scan_in_progress) {
        ESP_LOGW(TAG, "A scan is already in progress...");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Scanning for WiFi networks... (dispatched to worker)");
    if (xTaskCreate(async_scan_worker_task, "wifi_scn_wrk", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WiFi scan worker task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

int wifi_sta_get_scan_results(wifi_ap_record_t *ap_records, uint16_t max_records)
{
    if (s_scan_in_progress) {
        return -1; // Still scanning
    }

    // Done scanning! Copy results to the caller safely without making ANY ESP-IDF API calls!
    uint16_t count_to_copy = (s_scan_count > max_records) ? max_records : s_scan_count;
    
    if (ap_records && count_to_copy > 0) {
        memcpy(ap_records, s_scan_results, sizeof(wifi_ap_record_t) * count_to_copy);
    }

    return (int)s_scan_count;
}
