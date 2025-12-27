#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    EventGroupHandle_t event_group;
    EventBits_t ready_bit;
} network_boot_config_t;

esp_err_t network_boot_start(const network_boot_config_t *config);

#ifdef __cplusplus
}
#endif
