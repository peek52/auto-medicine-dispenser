#include "ds3231.h"
#include "i2c_manager.h"
#include "config.h"
#include "esp_log.h"
#include <stdio.h>

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

    // DS3231 time registers start at 0x00: seconds, minutes, hours
    uint8_t regs[3];
    esp_err_t ret = i2c_manager_read_reg(ADDR_DS3231, 0x00, regs, 3);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(ret));
        snprintf(buf, buf_len, "--:--:--");
        return ret;
    }

    uint8_t sec = bcd2dec(regs[0] & 0x7F);
    uint8_t min = bcd2dec(regs[1] & 0x7F);
    uint8_t hr  = bcd2dec(regs[2] & 0x3F);  // mask 24h mode bits

    snprintf(buf, buf_len, "%02d:%02d:%02d", hr, min, sec);
    return ESP_OK;
}

esp_err_t ds3231_get_date_str(char *buf, size_t buf_len)
{
    if (!buf || buf_len < 16) return ESP_ERR_INVALID_ARG;

    // DS3231 date registers: 0x03=day-of-week, 0x04=date, 0x05=month, 0x06=year
    uint8_t regs[4];
    esp_err_t ret = i2c_manager_read_reg(ADDR_DS3231, 0x03, regs, 4);
    if (ret != ESP_OK) {
        buf[0] = '\0';
        return ret;
    }

    static const char *days[] = { "", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    uint8_t dow   = regs[0] & 0x07;                     // 1=Sun … 7=Sat
    uint8_t date  = bcd2dec(regs[1] & 0x3F);
    uint8_t month = bcd2dec(regs[2] & 0x1F);
    uint8_t year  = bcd2dec(regs[3]);

    if (dow < 1 || dow > 7 || date < 1 || date > 31 || month < 1 || month > 12 || year == 0) {
        buf[0] = '\0';
        ESP_LOGW(TAG, "invalid date registers dow=%u date=%u month=%u year=%u", dow, date, month, year);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *day_name = (dow >= 1 && dow <= 7) ? days[dow] : "---";
    snprintf(buf, buf_len, "%s %02d/%02d/20%02d", day_name, date, month, year);
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
