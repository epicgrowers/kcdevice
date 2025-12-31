#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sensors/pipeline.h"
#include "network/network_boot.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    EventGroupHandle_t event_group;
} boot_coordinator_t;

typedef sensor_pipeline_launch_ctx_t boot_sensor_launch_ctx_t;

esp_err_t boot_coordinator_init(boot_coordinator_t *coordinator);

EventGroupHandle_t boot_coordinator_get_event_group(const boot_coordinator_t *coordinator);

void boot_coordinator_prepare_sensor_pipeline(const boot_coordinator_t *coordinator,
                                              EventBits_t ready_bit,
                                              uint32_t reading_interval_sec,
                                              sensor_pipeline_launch_ctx_t *out_ctx);

void boot_coordinator_configure_network_boot(const boot_coordinator_t *coordinator,
                                             EventBits_t ready_bit,
                                             EventBits_t degraded_bit,
                                             network_boot_config_t *out_config);

EventBits_t boot_coordinator_wait_bits(const boot_coordinator_t *coordinator,
                                       EventBits_t bits,
                                       bool wait_for_all_bits,
                                       TickType_t timeout_ticks);

esp_err_t boot_coordinator_launch_sensors_now(boot_sensor_launch_ctx_t *ctx);

void boot_coordinator_launch_sensors_async(void *ctx);

#ifdef __cplusplus
}
#endif
