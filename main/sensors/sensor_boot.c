#include "sensor_boot.h"

#include "boot/boot_config.h"
#include "boot/boot_monitor.h"
#include "freertos/task.h"
#include "platform/i2c_scanner.h"
#include "sensor_manager.h"
#include "sensors/config.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "SENSOR_BOOT";

static sensor_boot_config_t s_sensor_boot_config;
static bool s_sensor_task_started = false;

static const sensor_discovery_rules_t *get_sensor_rules(void)
{
    return sensor_config_get_discovery_rules();
}

static uint32_t get_read_interval_sec(void)
{
    return (s_sensor_boot_config.reading_interval_sec == 0)
               ? SENSOR_BOOT_DEFAULT_READING_INTERVAL_SEC
               : s_sensor_boot_config.reading_interval_sec;
}

static uint32_t get_init_retry_count(void)
{
    const uint32_t configured = get_sensor_rules()->sensor_init_retry_count;
    return (configured == 0) ? 1U : configured;
}

static void append_csv(char *buffer, size_t buffer_len, const char *entry)
{
    if (buffer == NULL || entry == NULL || buffer_len == 0) {
        return;
    }

    size_t current_len = strlen(buffer);
    if (current_len >= buffer_len - 1) {
        return;
    }

    if (current_len > 0) {
        if (current_len + 2 >= buffer_len - 1) {
            return;
        }
        buffer[current_len++] = ',';
        buffer[current_len++] = ' ';
        buffer[current_len] = '\0';
    }

    size_t remaining = buffer_len - current_len - 1;
    strncat(buffer, entry, remaining);
}

static void sensor_task(void *arg)
{
    (void)arg;
    const sensor_discovery_rules_t *rules = get_sensor_rules();

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "SENSOR TASK: Starting");
    ESP_LOGI(TAG, "========================================");

    if (rules->i2c_stabilization_delay_ms > 0) {
        ESP_LOGI(TAG, "SENSOR: Waiting %lums for I2C sensor stabilization...",
                 (unsigned long)rules->i2c_stabilization_delay_ms);
        vTaskDelay(pdMS_TO_TICKS(rules->i2c_stabilization_delay_ms));
    }

    ESP_LOGI(TAG, "SENSOR: Performing initial I2C scan...");
    esp_err_t ret = i2c_scanner_scan();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SENSOR: I2C scan encountered errors: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "SENSOR: Initializing sensor manager...");
    int init_attempts = 0;
    const uint32_t max_attempts = get_init_retry_count();
    while (init_attempts < max_attempts) {
        ret = sensor_manager_init();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "SENSOR: \u2713 Sensor manager initialized successfully");
            break;
        }

        init_attempts++;
        if (init_attempts < max_attempts) {
            const uint32_t retry_delay_ms = rules->sensor_init_retry_delay_ms;
            ESP_LOGW(TAG, "SENSOR: Init attempt %d/%lu failed, retrying in %lums...",
                     init_attempts, (unsigned long)max_attempts, (unsigned long)retry_delay_ms);
            if (retry_delay_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
            }
        } else {
            ESP_LOGW(TAG, "SENSOR: Failed to initialize sensor manager after %lu attempts",
                     (unsigned long)max_attempts);
            ESP_LOGW(TAG, "SENSOR: Continuing with no sensors (graceful degradation)");
        }
    }

    ESP_LOGI(TAG, "SENSOR: Performing final I2C verification scan...");
    i2c_scanner_scan();

    char found_list[192] = {0};
    const uint8_t total_ezo = sensor_manager_get_ezo_count();

    for (uint8_t idx = 0; idx < total_ezo; idx++) {
        sensor_manager_ezo_info_t info = {0};
        if (!sensor_manager_get_ezo_info(idx, &info)) {
            continue;
        }

        char entry[32];
        snprintf(entry, sizeof(entry), "%s (0x%02X)", info.type, info.address);
        append_csv(found_list, sizeof(found_list), entry);
    }

    const bool battery_present = sensor_manager_has_battery_monitor();
    const char *found_summary = (found_list[0] != '\0') ? found_list : "None detected";

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "SENSOR INVENTORY SUMMARY");
    ESP_LOGI(TAG, "  Battery Monitor: %s", battery_present ? "YES" : "NO");
    ESP_LOGI(TAG, "  EZO Sensors: %u", total_ezo);
    ESP_LOGI(TAG, "  Detected Sensors: %s", found_summary);
    ESP_LOGI(TAG, "========================================");

    char telemetry_detail[192];
    snprintf(telemetry_detail, sizeof(telemetry_detail),
             "count=%u | battery=%s | sensors=%.120s",
             total_ezo,
             battery_present ? "yes" : "no",
             found_summary);
    boot_monitor_publish("sensors_ready", telemetry_detail);

    ESP_LOGI(TAG, "SENSOR: Starting sensor reading task...");
    ret = sensor_manager_start_reading_task(get_read_interval_sec());
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SENSOR: Failed to start reading task: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "SENSOR: \u2713 Sensor task complete, signaling SENSORS_READY");
    if (s_sensor_boot_config.event_group != NULL && s_sensor_boot_config.ready_bit != 0) {
        xEventGroupSetBits(s_sensor_boot_config.event_group, s_sensor_boot_config.ready_bit);
    }

    vTaskDelete(NULL);
}

esp_err_t sensor_boot_start(const sensor_boot_config_t *config)
{
    if (config == NULL || config->event_group == NULL || config->ready_bit == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_sensor_task_started) {
        return ESP_OK;
    }

    s_sensor_boot_config = *config;
    if (s_sensor_boot_config.reading_interval_sec == 0) {
        s_sensor_boot_config.reading_interval_sec = SENSOR_BOOT_DEFAULT_READING_INTERVAL_SEC;
    }

    BaseType_t task_ret = xTaskCreate(
        sensor_task,
        "sensor_task",
        SENSOR_TASK_STACK_SIZE,
        NULL,
        SENSOR_TASK_PRIORITY,
        NULL);

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sensor task");
        return ESP_FAIL;
    }

    s_sensor_task_started = true;
    ESP_LOGI(TAG, "SENSOR: sensor_task launched (priority %d)", SENSOR_TASK_PRIORITY);
    return ESP_OK;
}
