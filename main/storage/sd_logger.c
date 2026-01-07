#include "storage/sd_logger.h"

#include "sdkconfig.h"
#include "esp_log.h"

#if CONFIG_KC_SD_LOGGER_ENABLED

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <time.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "sensors/sensor_manager.h"
#include "services/logging/log_history.h"

#define SD_LOGGER_TASK_NAME           "sd_logger"
#define SD_LOGGER_TASK_STACK_WORDS    4096
#define SD_LOGGER_TASK_PRIORITY       4
#define SD_LOGGER_MOUNT_POINT         "/sdcard"
#define SD_LOGGER_LOG_DIR             SD_LOGGER_MOUNT_POINT "/logs"
#define SD_LOGGER_FILENAME_TEMPLATE   SD_LOGGER_LOG_DIR "/%Y%m%d.ndjson"
#define SD_LOGGER_MIN_INTERVAL_SEC    5U
#define SD_LOGGER_RETRY_INTERVAL_MS   10000

static const char *TAG = "SD_LOGGER";

static TaskHandle_t s_logger_task_handle = NULL;
static SemaphoreHandle_t s_file_mutex = NULL;
static sdmmc_card_t *s_card = NULL;
static bool s_sd_mounted = false;
static FILE *s_log_file = NULL;
static int s_open_day_key = -1;
static int64_t s_last_mount_attempt_us = 0;
static bool s_card_reported_missing = false;
static bool s_spi_bus_initialized = false;

static esp_err_t sd_logger_mount_card(void);
static void sd_logger_unmount_card(void);
static bool sd_logger_card_present(void);
static void sd_logger_task(void *arg);
static uint32_t sd_logger_interval_ms(void);
static esp_err_t sd_logger_prepare_log_path_locked(const struct tm *utc_time);
static esp_err_t sd_logger_write_entry_locked(const sensor_cache_t *cache,
                                              time_t timestamp);
static void sd_logger_add_sensor_payload(cJSON *sensors, const cached_sensor_t *sensor);
static void sd_logger_close_file_locked(void);

static uint32_t sd_logger_interval_ms(void)
{
    uint32_t interval = CONFIG_KC_SD_LOG_INTERVAL_SEC;
    if (interval < SD_LOGGER_MIN_INTERVAL_SEC) {
        interval = SD_LOGGER_MIN_INTERVAL_SEC;
    }
    return interval * 1000U;
}

static void sd_logger_configure_card_detect(void)
{
#if CONFIG_KC_SD_CARD_DETECT_GPIO >= 0
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_KC_SD_CARD_DETECT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = true,
        .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
#endif
}

static bool sd_logger_card_present(void)
{
#if CONFIG_KC_SD_CARD_DETECT_GPIO >= 0
    int level = gpio_get_level(CONFIG_KC_SD_CARD_DETECT_GPIO);
#if CONFIG_KC_SD_CARD_DETECT_ACTIVE_LOW
    return level == 0;
#else
    return level == 1;
#endif
#else
    return true;
#endif
}

static void sd_logger_report_card_state(bool inserted)
{
    if (inserted) {
        if (s_card_reported_missing) {
            ESP_LOGI(TAG, "SD card detected");
            s_card_reported_missing = false;
        }
        return;
    }

    if (!s_card_reported_missing) {
        ESP_LOGW(TAG, "SD card not detected, logging paused until insertion");
        s_card_reported_missing = true;
    }
}

static esp_err_t sd_logger_mount_card(void)
{
    if (s_sd_mounted) {
        return ESP_OK;
    }

    if (!sd_logger_card_present()) {
        sd_logger_report_card_state(false);
        return ESP_ERR_INVALID_STATE;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = (spi_host_device_t)CONFIG_KC_SD_SPI_HOST;
    host.max_freq_khz = CONFIG_KC_SD_SPI_MAX_FREQ_KHZ;

    bool bus_initialized_this_call = false;
    if (!s_spi_bus_initialized) {
        const spi_bus_config_t bus_config = {
            .mosi_io_num = CONFIG_KC_SD_SPI_MOSI_GPIO,
            .miso_io_num = CONFIG_KC_SD_SPI_MISO_GPIO,
            .sclk_io_num = CONFIG_KC_SD_SPI_CLK_GPIO,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 16 * 1024,
        };

        esp_err_t bus_ret = spi_bus_initialize(host.slot, &bus_config, SDSPI_DEFAULT_DMA);
        if (bus_ret == ESP_ERR_INVALID_STATE) {
            s_spi_bus_initialized = true;
        } else if (bus_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(bus_ret));
            return bus_ret;
        } else {
            s_spi_bus_initialized = true;
            bus_initialized_this_call = true;
        }
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = CONFIG_KC_SD_SPI_CS_GPIO;
#if CONFIG_KC_SD_CARD_DETECT_GPIO >= 0
    slot_config.gpio_cd = CONFIG_KC_SD_CARD_DETECT_GPIO;
#endif
    slot_config.host_id = host.slot;

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 0,
    };

    int64_t now = esp_timer_get_time();
    s_last_mount_attempt_us = now;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(SD_LOGGER_MOUNT_POINT,
                                            &host,
                                            &slot_config,
                                            &mount_config,
                                            &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        if (bus_initialized_this_call) {
            spi_bus_free(host.slot);
            s_spi_bus_initialized = false;
        }
        return ret;
    }

    s_sd_mounted = true;
    s_open_day_key = -1;
    sd_logger_report_card_state(true);

    uint64_t capacity_bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    ESP_LOGI(TAG,
             "Mounted SD card: %s %uMB",
             s_card->cid.name,
             (unsigned)(capacity_bytes / (1024 * 1024)));
    return ESP_OK;
}

static void sd_logger_unmount_card(void)
{
    if (!s_sd_mounted) {
        return;
    }

    sd_logger_close_file_locked();
    esp_vfs_fat_sdcard_unmount(SD_LOGGER_MOUNT_POINT, s_card);
    s_card = NULL;
    s_sd_mounted = false;
    s_open_day_key = -1;
    ESP_LOGW(TAG, "SD card unmounted");

    if (s_spi_bus_initialized) {
        esp_err_t bus_ret = spi_bus_free((spi_host_device_t)CONFIG_KC_SD_SPI_HOST);
        if (bus_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to free SPI bus: %s", esp_err_to_name(bus_ret));
        } else {
            s_spi_bus_initialized = false;
        }
    }
}

static void sd_logger_close_file_locked(void)
{
    if (s_log_file != NULL) {
        fflush(s_log_file);
        fclose(s_log_file);
        s_log_file = NULL;
    }
}

static esp_err_t sd_logger_ensure_log_dir_locked(void)
{
    struct stat st = {0};
    if (stat(SD_LOGGER_LOG_DIR, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Log path exists but is not a directory: %s", SD_LOGGER_LOG_DIR);
        return ESP_FAIL;
    }

    if (mkdir(SD_LOGGER_LOG_DIR, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to create log directory (%d)", errno);
    return ESP_FAIL;
}

static int sd_logger_compute_day_key(const struct tm *utc_time)
{
    if (utc_time == NULL) {
        return -1;
    }
    return (utc_time->tm_year + 1900) * 1000 + utc_time->tm_yday;
}

static esp_err_t sd_logger_prepare_log_path_locked(const struct tm *utc_time)
{
    if (utc_time == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    int day_key = sd_logger_compute_day_key(utc_time);
    if (s_log_file != NULL && day_key == s_open_day_key) {
        return ESP_OK;
    }

    sd_logger_close_file_locked();

    esp_err_t dir_ret = sd_logger_ensure_log_dir_locked();
    if (dir_ret != ESP_OK) {
        return dir_ret;
    }

    char path[128];
    size_t written = strftime(path, sizeof(path), SD_LOGGER_FILENAME_TEMPLATE, utc_time);
    if (written == 0 || written >= sizeof(path)) {
        ESP_LOGE(TAG, "Failed to build log path");
        return ESP_FAIL;
    }

    s_log_file = fopen(path, "a");
    if (s_log_file == NULL) {
        ESP_LOGE(TAG, "Failed to open log file %s (errno=%d)", path, errno);
        return ESP_FAIL;
    }

    s_open_day_key = day_key;
    ESP_LOGI(TAG, "Logging sensor snapshots to %s", path);
    return ESP_OK;
}

static void sd_logger_add_sensor_payload(cJSON *sensors, const cached_sensor_t *sensor)
{
    if (sensors == NULL || sensor == NULL || !sensor->valid) {
        return;
    }

    if (sensor->value_count == 1) {
        cJSON_AddNumberToObject(sensors, sensor->sensor_type, sensor->values[0]);
        return;
    }

    cJSON *sensor_obj = cJSON_CreateObject();
    if (sensor_obj == NULL) {
        return;
    }

    if (strcmp(sensor->sensor_type, "HUM") == 0) {
        if (sensor->value_count >= 1) cJSON_AddNumberToObject(sensor_obj, "humidity", sensor->values[0]);
        if (sensor->value_count >= 2) cJSON_AddNumberToObject(sensor_obj, "air_temp", sensor->values[1]);
        if (sensor->value_count >= 3) cJSON_AddNumberToObject(sensor_obj, "dew_point", sensor->values[2]);
    } else if (strcmp(sensor->sensor_type, "EC") == 0) {
        if (sensor->value_count >= 1) cJSON_AddNumberToObject(sensor_obj, "conductivity", sensor->values[0]);
        if (sensor->value_count >= 2) cJSON_AddNumberToObject(sensor_obj, "tds", sensor->values[1]);
        if (sensor->value_count >= 3) cJSON_AddNumberToObject(sensor_obj, "salinity", sensor->values[2]);
        if (sensor->value_count >= 4) cJSON_AddNumberToObject(sensor_obj, "specific_gravity", sensor->values[3]);
    } else if (strcmp(sensor->sensor_type, "DO") == 0) {
        if (sensor->value_count >= 1) cJSON_AddNumberToObject(sensor_obj, "dissolved_oxygen", sensor->values[0]);
        if (sensor->value_count >= 2) cJSON_AddNumberToObject(sensor_obj, "saturation", sensor->values[1]);
    } else {
        for (uint8_t i = 0; i < sensor->value_count && i < MAX_SENSOR_VALUES; ++i) {
            char key[8];
            snprintf(key, sizeof(key), "v%u", i);
            cJSON_AddNumberToObject(sensor_obj, key, sensor->values[i]);
        }
    }

    cJSON_AddItemToObject(sensors, sensor->sensor_type, sensor_obj);
}

static esp_err_t sd_logger_write_entry_locked(const sensor_cache_t *cache, time_t timestamp)
{
    if (cache == NULL || s_log_file == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    struct tm utc_time = {0};
    gmtime_r(&timestamp, &utc_time);

    char iso_time[32] = "";
    if (strftime(iso_time, sizeof(iso_time), "%Y-%m-%dT%H:%M:%SZ", &utc_time) == 0) {
        strlcpy(iso_time, "1970-01-01T00:00:00Z", sizeof(iso_time));
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "timestamp", iso_time);
    cJSON_AddNumberToObject(root, "reading_interval_sec", sensor_manager_get_reading_interval());
    cJSON_AddNumberToObject(root, "rssi", cache->rssi);
    cJSON_AddNumberToObject(root, "sensor_count", cache->sensor_count);
    if (cache->battery_valid) {
        cJSON_AddNumberToObject(root, "battery", cache->battery_percentage);
    } else {
        cJSON_AddNullToObject(root, "battery");
    }

    if (cache->timestamp_us > 0) {
        int64_t now_us = esp_timer_get_time();
        int64_t age_ms = (now_us - (int64_t)cache->timestamp_us) / 1000;
        if (age_ms < 0) {
            age_ms = 0;
        }
        cJSON_AddNumberToObject(root, "cache_age_ms", age_ms);
    }

    cJSON *sensors = cJSON_CreateObject();
    if (sensors == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    for (uint8_t i = 0; i < cache->sensor_count && i < 8; ++i) {
        sd_logger_add_sensor_payload(sensors, &cache->sensors[i]);
    }
    cJSON_AddItemToObject(root, "sensors", sensors);

    char *json_line = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_line == NULL) {
        return ESP_ERR_NO_MEM;
    }

    log_history_record_sensor_json(json_line);

    int written = fprintf(s_log_file, "%s\n", json_line);
    free(json_line);
    fflush(s_log_file);

    if (written < 0) {
        ESP_LOGE(TAG, "Failed to write log entry (errno=%d)", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool sd_logger_should_retry_mount(void)
{
    int64_t now = esp_timer_get_time();
    return (now - s_last_mount_attempt_us) >= (SD_LOGGER_RETRY_INTERVAL_MS * 1000LL);
}

static void sd_logger_process_cycle(void)
{
    sensor_cache_t cache = {0};
    if (sensor_manager_get_cached_data(&cache) != ESP_OK || !cache.sensor_count) {
        return;
    }

    bool card_inserted = sd_logger_card_present();
    sd_logger_report_card_state(card_inserted);
    if (!card_inserted) {
        if (s_sd_mounted) {
            sd_logger_unmount_card();
        }
        return;
    }

    if (!s_sd_mounted) {
        if (!sd_logger_should_retry_mount()) {
            return;
        }
        if (sd_logger_mount_card() != ESP_OK) {
            return;
        }
    }

    time_t now = time(NULL);
    struct tm utc_time = {0};
    if (gmtime_r(&now, &utc_time) == NULL) {
        memset(&utc_time, 0, sizeof(utc_time));
    }

    if (xSemaphoreTake(s_file_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "SD logger mutex timeout");
        return;
    }

    esp_err_t prep_ret = sd_logger_prepare_log_path_locked(&utc_time);
    if (prep_ret == ESP_OK) {
        esp_err_t write_ret = sd_logger_write_entry_locked(&cache, now);
        if (write_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to write SD log entry: %s", esp_err_to_name(write_ret));
        }
    }

    xSemaphoreGive(s_file_mutex);
}

static void sd_logger_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG,
             "SD logger task started (interval=%lus)",
             (unsigned long)CONFIG_KC_SD_LOG_INTERVAL_SEC);

    const uint32_t interval_ms = sd_logger_interval_ms();
    while (true) {
        sd_logger_process_cycle();
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
}

esp_err_t sd_logger_init(void)
{
    if (s_logger_task_handle != NULL) {
        return ESP_OK;
    }

    sd_logger_configure_card_detect();

    s_file_mutex = xSemaphoreCreateMutex();
    if (s_file_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create SD logger mutex");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ret = xTaskCreate(
        sd_logger_task,
        SD_LOGGER_TASK_NAME,
        SD_LOGGER_TASK_STACK_WORDS,
        NULL,
        SD_LOGGER_TASK_PRIORITY,
        &s_logger_task_handle);

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to start SD logger task");
        vSemaphoreDelete(s_file_mutex);
        s_file_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "SD logger initialized (task=%p)", (void *)s_logger_task_handle);
    return ESP_OK;
}

void sd_logger_shutdown(void)
{
    if (s_logger_task_handle != NULL) {
        vTaskDelete(s_logger_task_handle);
        s_logger_task_handle = NULL;
    }

    if (s_file_mutex != NULL) {
        xSemaphoreTake(s_file_mutex, portMAX_DELAY);
        sd_logger_close_file_locked();
        xSemaphoreGive(s_file_mutex);
        vSemaphoreDelete(s_file_mutex);
        s_file_mutex = NULL;
    }

    sd_logger_unmount_card();
}

#else  // CONFIG_KC_SD_LOGGER_ENABLED

esp_err_t sd_logger_init(void)
{
    ESP_LOGI("SD_LOGGER", "SD logging disabled (CONFIG_KC_SD_LOGGER_ENABLED=n)");
    return ESP_ERR_NOT_SUPPORTED;
}

void sd_logger_shutdown(void)
{
}

#endif  // CONFIG_KC_SD_LOGGER_ENABLED
