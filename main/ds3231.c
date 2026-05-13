#include "ds3231.h"
#include "i2c_manager.h"
#include "config.h"
#include "esp_log.h"
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

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

esp_err_t ds3231_seed_system_time(void)
{
    // Read seven registers starting at 0x00 (sec, min, hr, dow, day, mo, yr).
    uint8_t buf[7] = {0};
    esp_err_t r = i2c_manager_read_reg(ADDR_DS3231, 0x00, buf, sizeof(buf));
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "RTC seed: read failed (%s) — system time stays at epoch until SNTP",
                 esp_err_to_name(r));
        return r;
    }

    struct tm t = {0};
    t.tm_sec  = bcd2dec(buf[0] & 0x7F);
    t.tm_min  = bcd2dec(buf[1] & 0x7F);
    t.tm_hour = bcd2dec(buf[2] & 0x3F);
    t.tm_wday = (buf[3] & 0x07) - 1;
    t.tm_mday = bcd2dec(buf[4] & 0x3F);
    t.tm_mon  = bcd2dec(buf[5] & 0x1F) - 1;
    t.tm_year = bcd2dec(buf[6]) + 100;  // RTC year = years since 2000; tm_year = since 1900

    // Sanity check: reject obviously-uninitialised RTC contents.
    if (t.tm_year < 124 /* 2024 */ || t.tm_year > 199 /* 2099 */ ||
        t.tm_mon < 0 || t.tm_mon > 11 ||
        t.tm_mday < 1 || t.tm_mday > 31) {
        ESP_LOGW(TAG, "RTC seed: rejecting implausible date %04d-%02d-%02d %02d:%02d:%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
        return ESP_ERR_INVALID_STATE;
    }

    // The user's timezone is set later in sync_time_task (TZ=ICT-7).
    // mktime() interprets the tm as local time, so make sure TZ is at
    // least set to ICT here too — otherwise system time would land in
    // a different offset until SNTP fixes it.
    setenv("TZ", "ICT-7", 1);
    tzset();
    time_t epoch = mktime(&t);
    if (epoch <= 0) return ESP_ERR_INVALID_STATE;

    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) return ESP_FAIL;

    ESP_LOGI(TAG, "RTC seeded system time: %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
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
