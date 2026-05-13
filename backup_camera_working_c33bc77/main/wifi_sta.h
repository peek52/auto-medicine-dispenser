#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * wifi_sta — WiFi Station with AP fallback.
 * Reads credentials from NVS; falls back to AP mode if connection fails.
 */

/** Initialise WiFi and connect. Blocks until IP acquired or AP started. */
esp_err_t wifi_sta_init(void);

/** Returns current IP as string ("0.0.0.0" if not connected) */
const char *wifi_sta_get_ip(void);

/** Returns current STA SSID if known, otherwise empty string */
const char *wifi_sta_get_ssid(void);

/** Returns true if STA is connected and has IP */
bool wifi_sta_connected(void);

/** Save new credentials to NVS */
esp_err_t wifi_sta_save_credentials(const char *ssid, const char *pass);

/** Dynamic reconnect to new WiFi without restart */
esp_err_t wifi_sta_reconnect(const char *ssid, const char *pass);

/** Forget current WiFi and fallback to AP setup mode */
void wifi_sta_forget(void);

/**
 * Start scanning for nearby WiFi networks asynchronously.
 * Returns ESP_OK if scan started successfully.
 */
esp_err_t wifi_sta_start_scan(void);

/**
 * Check if scan is complete and return results.
 * Returns the number of APs found, or -1 if scan is still ongoing.
 */
int wifi_sta_get_scan_results(wifi_ap_record_t *ap_records, uint16_t max_records);

#ifdef __cplusplus
}
#endif
