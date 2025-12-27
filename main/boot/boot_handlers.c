#include "boot/boot_handlers.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "provisioning/provisioning_runner.h"
#include "nvs_flash.h"

static const char *TAG = "BOOT_HANDLERS";

void boot_provisioning_state_change_handler(provisioning_state_t state,
                                            provisioning_status_code_t status,
                                            const char *message)
{
    ESP_LOGI(TAG, "State changed: %s | Status: %s | Message: %s",
             provisioning_state_to_string(state),
             provisioning_status_to_string(status),
             message ? message : "N/A");
}

void boot_reset_button_handler(reset_button_event_t event, uint32_t press_duration_ms)
{
    switch (event) {
        case RESET_BUTTON_EVENT_SHORT_PRESS:
            ESP_LOGW(TAG, "====================================");
            ESP_LOGW(TAG, "SHORT PRESS DETECTED (%lu ms)", (unsigned long)press_duration_ms);
            ESP_LOGW(TAG, "Clearing WiFi credentials...");
            ESP_LOGW(TAG, "====================================");

            esp_err_t ret = provisioning_clear_saved_network();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "WiFi credentials cleared successfully");
                ESP_LOGI(TAG, "Restarting device to begin reprovisioning...");

                provisioning_disconnect();
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            } else {
                ESP_LOGE(TAG, "Failed to clear credentials: %s", esp_err_to_name(ret));
            }
            break;

        case RESET_BUTTON_EVENT_LONG_PRESS:
            ESP_LOGW(TAG, "====================================");
            ESP_LOGW(TAG, "LONG PRESS DETECTED (%lu ms)", (unsigned long)press_duration_ms);
            ESP_LOGW(TAG, "Performing FACTORY RESET...");
            ESP_LOGW(TAG, "====================================");

            ret = nvs_flash_erase();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "NVS erased successfully (factory reset)");
                ESP_LOGI(TAG, "Restarting device...");

                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            } else {
                ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(ret));
            }
            break;

        default:
            ESP_LOGW(TAG, "Unknown button event: %d", event);
            break;
    }
}
