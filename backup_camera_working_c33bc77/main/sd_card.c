#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#include "config.h"
#include "sd_card.h"

static const char *TAG = "sd_card";
static bool s_sd_card_mounted = false;
static sdmmc_card_t *s_sd_card = NULL;

static void sd_card_power_on(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SD_CARD_POWER_PIN) | (1ULL << 46),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    // Waveshare ESP32-P4-NANO schematic shows SD1_VDD switched by AO3401 on GPIO45.
    // Pull the P-MOS gate low to enable card power.
    // Some user reports for this board family mention GPIO46 on certain revisions,
    // so keep both low while diagnosing.
    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)SD_CARD_POWER_PIN, 0));
    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)46, 0));
    ESP_LOGI(TAG, "SD power enable asserted on GPIO%d and GPIO46 (active-low)", SD_CARD_POWER_PIN);
}

static esp_err_t sd_card_host_init_shared(void)
{
    esp_err_t err = sdmmc_host_init();
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "SDMMC host already initialized, reusing shared host");
        return ESP_OK;
    }
    return err;
}

static esp_err_t sd_card_host_deinit_shared(void)
{
    // The SDMMC host is shared with esp_hosted Wi-Fi. Do not deinit it here.
    return ESP_OK;
}

static esp_err_t sd_card_try_mount(int width)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    host.init = sd_card_host_init_shared;
    host.deinit = sd_card_host_deinit_shared;

    sdmmc_slot_config_t slot_config = {0};
    // ESP32-P4 slot 0 uses dedicated SDMMC IOs.
    // Keep the pin fields at 0 so the driver stays in native IOMUX mode.
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;
    slot_config.width = width;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting SD card at %s (slot=%d, width=%d)", SD_CARD_MOUNT_POINT, host.slot, width);
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_CARD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_sd_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (width=%d): %s", width, esp_err_to_name(err));
        s_sd_card = NULL;
        return err;
    }

    s_sd_card_mounted = true;
    ESP_LOGI(TAG, "SD card mounted with width=%d", width);
    sdmmc_card_print_info(stdout, s_sd_card);
    return ESP_OK;
}

static esp_err_t sd_card_write_probe_file(void)
{
    FILE *fp = fopen(SD_CARD_MOUNT_POINT "/sd_ready.txt", "a");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Failed to open %s", SD_CARD_MOUNT_POINT "/sd_ready.txt");
        return ESP_FAIL;
    }

    fprintf(fp, "mounted_ok uptime_ms=%lld\n", (long long)(esp_timer_get_time() / 1000));
    fclose(fp);
    return ESP_OK;
}

esp_err_t sd_card_init(void)
{
    if (s_sd_card_mounted) {
        return ESP_OK;
    }

    sd_card_power_on();

    esp_err_t err = sd_card_try_mount(4);
    if (err != ESP_OK) {
        err = sd_card_try_mount(1);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SD card not available after 4-bit and 1-bit retries");
            return err;
        }
    }

    err = sd_card_write_probe_file();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD probe write failed: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

bool sd_card_is_mounted(void)
{
    return s_sd_card_mounted;
}

const char *sd_card_mount_point(void)
{
    return SD_CARD_MOUNT_POINT;
}
