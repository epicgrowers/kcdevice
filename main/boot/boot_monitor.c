#include "boot_monitor.h"

#include "esp_log.h"
#include "services/telemetry/mqtt_telemetry.h"
#include <stdio.h>

static const char *TAG = "MAIN:BOOT:MONITOR";

void boot_monitor_publish(const char *stage, const char *detail)
{
    if (stage == NULL) {
        return;
    }

    if (!mqtt_client_is_connected()) {
        ESP_LOGD(TAG, "MQTT not ready, skipping stage publish: %s", stage);
        return;
    }

    char payload[160];
    if (detail != NULL && detail[0] != '\0') {
        snprintf(payload, sizeof(payload), "%s | %s", stage, detail);
    } else {
        snprintf(payload, sizeof(payload), "%s", stage);
    }

    esp_err_t err = mqtt_publish_status(payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish stage %s: %s", stage, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Published boot stage: %s", payload);
    }
}
