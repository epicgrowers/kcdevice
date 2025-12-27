#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "chip_info.h"
#include "provisioning/provisioning_state.h"
#include "security.h"
#include "reset_button.h"
#include "i2c_scanner.h"
#include "boot/boot_config.h"
#include "boot/boot_coordinator.h"
#include "boot/boot_handlers.h"
#include "provisioning/provisioning_runner.h"
#include "network/network_boot.h"
#include "sensors/pipeline.h"

static const char *TAG = "MAIN";

static boot_coordinator_t s_boot_coordinator = {0};

/**
 * @brief Main application entry point
 * Implements parallel boot architecture with sensor and network tasks
 */
void app_main(void)
{
    ESP_LOGI(TAG, "KannaCloud Device - Parallel Boot v2.0");
    
    // Enable verbose logging for provisioning components
    esp_log_level_set("wifi_prov_mgr", ESP_LOG_DEBUG);
    esp_log_level_set("protocomm", ESP_LOG_DEBUG);
    esp_log_level_set("wifi_prov_scheme_ble", ESP_LOG_DEBUG);
    
    // Log chip information
    chip_info_log();
    
    // Phase 1: Basic hardware initialization
    ESP_LOGI(TAG, "MAIN: Initializing basic hardware...");
    
    // Initialize security features (NVS encryption with eFuse protection)
    esp_err_t ret = security_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MAIN: Security initialization failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "MAIN: Device will continue but credentials may not be secure!");
    }
    
    // Initialize reset button (GPIO0 - BOOT button)
    ret = reset_button_init(RESET_BUTTON_GPIO, boot_reset_button_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MAIN: Failed to initialize reset button: %s", esp_err_to_name(ret));
    }
    
    // Initialize I2C bus hardware (required for sensors)
    ESP_LOGI(TAG, "MAIN: Initializing I2C bus...");
    ret = i2c_scanner_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MAIN: I2C bus initialization failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "MAIN: Restarting device (hardware issue)...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
    
    // Initialize provisioning state machine
    provisioning_state_init();
    provisioning_state_register_callback(boot_provisioning_state_change_handler);
    
    ret = boot_coordinator_init(&s_boot_coordinator);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MAIN: Failed to initialize boot coordinator: %s", esp_err_to_name(ret));
        esp_restart();
    }

    network_boot_config_t network_boot_cfg = {0};
    boot_coordinator_configure_network_boot(&s_boot_coordinator, NETWORK_READY_BIT, &network_boot_cfg);
    
    // Phase 2: Wi-Fi connection (required for network services)
    boot_sensor_launch_ctx_t sensor_launch_ctx = {0};
    boot_coordinator_prepare_sensor_pipeline(
        &s_boot_coordinator,
        SENSORS_READY_BIT,
        SENSOR_PIPELINE_DEFAULT_INTERVAL_SEC,
        &sensor_launch_ctx);

    const provisioning_plan_t provisioning_plan = {
        .sensor_event_group = boot_coordinator_get_event_group(&s_boot_coordinator),
        .sensors_ready_bit = SENSORS_READY_BIT,
        .sensor_log_interval_ms = PROVISIONING_SENSOR_LOG_INTERVAL_MS,
        .on_parallel_work_start = boot_coordinator_launch_sensors_async,
        .on_parallel_work_ctx = &sensor_launch_ctx,
        .attempt_stored_credentials_first = true,
    };

    provisioning_outcome_t provisioning_outcome;
    ret = provisioning_run(&provisioning_plan, &provisioning_outcome);
    if (ret != ESP_OK || !provisioning_outcome.connected) {
        ESP_LOGE(TAG, "MAIN: Provisioning run failed: %s", esp_err_to_name(ret));
        return;
    }

    if (provisioning_outcome.used_stored_credentials) {
        ESP_LOGI(TAG, "MAIN: WiFi connected using stored credentials");
    }

    if (provisioning_outcome.provisioning_triggered) {
        if (provisioning_outcome.sensors_ready_during_provisioning) {
            ESP_LOGI(TAG, "MAIN: \u2713 Sensors initialized during provisioning (data already available)");
        } else {
            ESP_LOGI(TAG, "MAIN: Sensors still initializing after provisioning");
        }
    }

    // Phase 3: Launch network task (+ sensors if not already launched)
    ESP_LOGI(TAG, "MAIN: Launching network services");
    
    // Launch sensor task if not already launched during provisioning
    // (This handles the case where credentials existed and provisioning was skipped)
    if (!sensor_launch_ctx.sensor_task_launched) {
        ESP_LOGI(TAG, "MAIN: Launching sensor task now...");
        esp_err_t sensor_start_ret = boot_coordinator_launch_sensors_now(&sensor_launch_ctx);
        if (sensor_start_ret != ESP_OK) {
            ESP_LOGE(TAG, "MAIN: Failed to start sensor task: %s", esp_err_to_name(sensor_start_ret));
        }
    }
    
    esp_err_t net_start_ret = network_boot_start(&network_boot_cfg);
    if (net_start_ret != ESP_OK) {
        ESP_LOGE(TAG, "MAIN: Failed to start network task: %s", esp_err_to_name(net_start_ret));
    } else {
        ESP_LOGI(TAG, "MAIN: ✓ Network task launch requested");
    }
    
    // Phase 4: Wait for boot tasks to complete
    ESP_LOGI(TAG, "MAIN: Waiting for sensor task to complete...");
    EventBits_t bits = boot_coordinator_wait_bits(
        &s_boot_coordinator,
        SENSORS_READY_BIT,
        false,
        pdMS_TO_TICKS(60000));

    if (bits & SENSORS_READY_BIT) {
        ESP_LOGI(TAG, "MAIN: ✓ Sensors ready");
    } else {
        ESP_LOGW(TAG, "MAIN: Sensor task timeout (60s), continuing anyway");
    }
    
    ESP_LOGI(TAG, "MAIN: Waiting up to %dms for network services (optional)...", NETWORK_READY_WAIT_MS);
    bits = boot_coordinator_wait_bits(
        &s_boot_coordinator,
        NETWORK_READY_BIT,
        false,
        pdMS_TO_TICKS(NETWORK_READY_WAIT_MS));

    if (bits & NETWORK_READY_BIT) {
        ESP_LOGI(TAG, "MAIN: ✓ Network services ready");
    } else {
        ESP_LOGW(TAG, "MAIN: Network services still initializing after %dms, continuing offline", NETWORK_READY_WAIT_MS);
    }
    
    // Phase 5: Enter normal operation mode
    ESP_LOGI(TAG, "MAIN: ✓ Boot complete, entering normal operation");
    
    // Main loop - monitor WiFi connection
    while (1) {
        provisioning_connection_guard_poll();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}




