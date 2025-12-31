#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sensors/pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const sensor_pipeline_launch_ctx_t *sensor_ctx;
} telemetry_pipeline_config_t;

typedef struct {
    uint32_t last_sequence_id;
    int64_t last_capture_us;
    int64_t last_publish_us;
    uint32_t total_attempts;
    uint32_t total_success;
    uint32_t total_failure;
    bool last_publish_success;
    uint8_t last_sensor_count;
    bool last_battery_valid;
    float last_battery_percent;
    int8_t last_rssi;
} telemetry_pipeline_metrics_t;

typedef struct {
    const sensor_pipeline_launch_ctx_t *sensor_ctx;
    uint32_t sequence_id;
    telemetry_pipeline_metrics_t metrics;
} telemetry_pipeline_t;

typedef struct {
    sensor_pipeline_snapshot_t snapshot;
    uint32_t sequence_id;
    int64_t captured_at_us;
    bool cache_valid;
} telemetry_pipeline_sample_t;

void telemetry_pipeline_init(telemetry_pipeline_t *pipeline,
                             const telemetry_pipeline_config_t *config);

esp_err_t telemetry_pipeline_acquire_sample(telemetry_pipeline_t *pipeline,
                                            telemetry_pipeline_sample_t *out_sample);

void telemetry_pipeline_log_publish_attempt(const telemetry_pipeline_sample_t *sample,
                                            const char *channel,
                                            const char *note);

void telemetry_pipeline_record_publish_result(telemetry_pipeline_t *pipeline,
                                              const telemetry_pipeline_sample_t *sample,
                                              bool success);

void telemetry_pipeline_get_metrics(const telemetry_pipeline_t *pipeline,
                                    telemetry_pipeline_metrics_t *out_metrics);

#ifdef __cplusplus
}
#endif
