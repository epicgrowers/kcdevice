#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sensors/sensor_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SENSOR_PIPELINE_DEFAULT_INTERVAL_SEC 10U

typedef struct {
    EventGroupHandle_t event_group;
    EventBits_t ready_bit;
    uint32_t reading_interval_sec;
    bool sensor_task_launched;
} sensor_pipeline_launch_ctx_t;

typedef struct {
    bool sensor_task_launched;
    bool sensors_ready;
    EventBits_t ready_bit;
    EventBits_t raw_event_bits;
    EventGroupHandle_t event_group;
} sensor_pipeline_readiness_t;

typedef struct {
    sensor_pipeline_readiness_t readiness;
    uint32_t reading_interval_sec;
    sensor_cache_t cache;
    bool cache_valid;
} sensor_pipeline_snapshot_t;

typedef void (*sensor_pipeline_snapshot_listener_t)(const sensor_pipeline_snapshot_t *snapshot,
                                                    void *user_ctx);

void sensor_pipeline_prepare(sensor_pipeline_launch_ctx_t *ctx,
                             EventGroupHandle_t event_group,
                             EventBits_t ready_bit,
                             uint32_t reading_interval_sec);

const sensor_pipeline_launch_ctx_t *sensor_pipeline_get_active_ctx(void);

esp_err_t sensor_pipeline_launch(sensor_pipeline_launch_ctx_t *ctx);

void sensor_pipeline_launch_async(void *ctx);

esp_err_t sensor_pipeline_snapshot(const sensor_pipeline_launch_ctx_t *ctx,
                                   sensor_pipeline_snapshot_t *out_snapshot);

esp_err_t sensor_pipeline_register_snapshot_listener(sensor_pipeline_launch_ctx_t *ctx,
                                                     sensor_pipeline_snapshot_listener_t listener,
                                                     void *user_ctx);

#ifdef __cplusplus
}
#endif
