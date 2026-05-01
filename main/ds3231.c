#include "ds3231.h"
#include "i2c_manager.h"
#include "config.h"
#include "esp_log.h"
#include <stdio.h>
#include <time.h>

static const char *TAG = "ds3231";

static uint8_t bcd2dec(uint8_t bcd) {
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

static uint8_t dec2bcd(uint8_t dec) {
    return ((dec / 10) << 4) + (dec % 10);
}

esp_err_t ds3231_get_time_str(char *buf, size_t buf_len)
{
    if (!buf || buf_len < 9) return ESP_ERR_INVALID_ARG;

    // Always render from system time. The DS3231 is read once at boot
    // (and every NTP resync) to seed/correct system time; reading it
    // every second over the shared I2C bus made the standby clock
    // alternate between the real time and garbled BCD bytes whenever
    // the bus state machine wedged. esp_timer-backed system time
    // can't garble — and is the same value the RTC would have given
    // anyway as long as the RTC seed at boot was good.
    time_t now = 0;
    struct tm tm = {0};
    time(&now);
    localtime_r(&now, &tm);
    snprintf(buf, buf_len, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return ESP_OK;
}

esp_err_t ds3231_get_date_str(char *buf, size_t buf_len)
{
    if (!buf || buf_len < 16) return ESP_ERR_INVALID_ARG;

    // Same reasoning as ds3231_get_time_str: render from system time
    // so a flaky shared I2C bus can't poison the standby date display.
    time_t now = 0;
    struct tm tm = {0};
    time(&now);
    localtime_r(&now, &tm);
    static const char *days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    int dow = tm.tm_wday;
    if (dow < 0 || dow > 6) dow = 0;
    snprintf(buf, buf_len, "%s %02d/%02d/%04d",
             days[dow], tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
    return ESP_OK;
}

esp_err_t ds3231_set_time(struct tm *timeinfo)
{
    if (!timeinfo) return ESP_ERR_INVALID_ARG;

    uint8_t data[8];
    data[0] = 0x00; // start at register 0
    data[1] = dec2bcd(timeinfo->tm_sec);
    data[2] = dec2bcd(timeinfo->tm_min);
    data[3] = dec2bcd(timeinfo->tm_hour);
    data[4] = timeinfo->tm_wday + 1; // 1-7 (Sunday=1)
    data[5] = dec2bcd(timeinfo->tm_mday);
    data[6] = dec2bcd(timeinfo->tm_mon + 1);
    data[7] = dec2bcd(timeinfo->tm_year % 100);

    return i2c_manager_write(ADDR_DS3231, data, 8);
}
