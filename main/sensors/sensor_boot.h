#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SENSOR_BOOT_DEFAULT_READING_INTERVAL_SEC 10U

typedef struct {
    EventGroupHandle_t event_group;
    EventBits_t ready_bit;
    uint32_t reading_interval_sec;
} sensor_boot_config_t;

/**
 * @brief Launch the high-priority sensor boot task if not already running.
 */
esp_err_t sensor_boot_start(const sensor_boot_config_t *config);

#ifdef __cplusplus
}
#endif
