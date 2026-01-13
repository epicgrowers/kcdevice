#include "services/telemetry/telemetry_pipeline.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "SERVICES:TELEMETRY";

static const sensor_pipeline_launch_ctx_t *resolve_ctx(const telemetry_pipeline_t *pipeline)
{
    if (pipeline != NULL && pipeline->sensor_ctx != NULL) {
        return pipeline->sensor_ctx;
    }
    return sensor_pipeline_get_active_ctx();
}

void telemetry_pipeline_init(telemetry_pipeline_t *pipeline,
                             const telemetry_pipeline_config_t *config)
{
    if (pipeline == NULL) {
        return;
    }

    pipeline->sensor_ctx = (config != NULL) ? config->sensor_ctx : NULL;
    pipeline->sequence_id = 0;
    memset(&pipeline->metrics, 0, sizeof(pipeline->metrics));
}

esp_err_t telemetry_pipeline_acquire_sample(telemetry_pipeline_t *pipeline,
                                            telemetry_pipeline_sample_t *out_sample)
{
    if (pipeline == NULL || out_sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_sample, 0, sizeof(*out_sample));

    const sensor_pipeline_launch_ctx_t *ctx = resolve_ctx(pipeline);
    if (ctx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t snapshot_ret = sensor_pipeline_snapshot(ctx, &out_sample->snapshot);
    out_sample->captured_at_us = esp_timer_get_time();
    out_sample->sequence_id = ++pipeline->sequence_id;
    out_sample->cache_valid = (snapshot_ret == ESP_OK) && out_sample->snapshot.cache_valid;

    telemetry_pipeline_metrics_t *metrics = &pipeline->metrics;
    metrics->last_sequence_id = out_sample->sequence_id;
    metrics->last_capture_us = out_sample->captured_at_us;
    metrics->last_sensor_count = out_sample->snapshot.cache.sensor_count;
    metrics->last_rssi = out_sample->snapshot.cache.rssi;
    metrics->last_battery_valid = out_sample->cache_valid &&
                                  out_sample->snapshot.cache.battery_valid;
    metrics->last_battery_percent = out_sample->snapshot.cache.battery_percentage;

    return snapshot_ret;
}

void telemetry_pipeline_log_publish_attempt(const telemetry_pipeline_sample_t *sample,
                                            const char *channel,
                                            const char *note)
{
    const char *log_channel = (channel != NULL) ? channel : "telemetry";
    const char *log_note = (note != NULL) ? note : "";

    if (sample == NULL) {
        ESP_LOGI(TAG, "[%s] no sample available (%s)", log_channel, log_note);
        return;
    }

    const sensor_pipeline_readiness_t *ready = &sample->snapshot.readiness;
    const sensor_cache_t *cache = &sample->snapshot.cache;
    char battery_str[16];
    if (sample->cache_valid && cache->battery_valid) {
        snprintf(battery_str, sizeof(battery_str), "%.1f%%", cache->battery_percentage);
    } else {
        snprintf(battery_str, sizeof(battery_str), "n/a");
    }

    const int64_t now_us = esp_timer_get_time();
    const int64_t age_ms = (sample->captured_at_us > 0)
                               ? (now_us - sample->captured_at_us) / 1000
                               : -1;

    ESP_LOGI(TAG,
             "[%s] sample=%lu ready=%d cache_valid=%d sensors=%u rssi=%d battery=%s age_ms=%lld note=%s",
             log_channel,
             (unsigned long)sample->sequence_id,
             ready->sensors_ready,
             sample->cache_valid,
             cache->sensor_count,
             cache->rssi,
             battery_str,
             (long long)age_ms,
             log_note);
}

void telemetry_pipeline_record_publish_result(telemetry_pipeline_t *pipeline,
                                              const telemetry_pipeline_sample_t *sample,
                                              bool success)
{
    if (pipeline == NULL) {
        return;
    }

    telemetry_pipeline_metrics_t *metrics = &pipeline->metrics;
    metrics->total_attempts++;
    metrics->last_publish_success = success;
    metrics->last_publish_us = esp_timer_get_time();

    if (success) {
        metrics->total_success++;
    } else {
        metrics->total_failure++;
    }

    if (sample != NULL) {
        metrics->last_sequence_id = sample->sequence_id;
        metrics->last_capture_us = sample->captured_at_us;
        metrics->last_sensor_count = sample->snapshot.cache.sensor_count;
        metrics->last_rssi = sample->snapshot.cache.rssi;
        metrics->last_battery_valid = sample->cache_valid &&
                                      sample->snapshot.cache.battery_valid;
        metrics->last_battery_percent = sample->snapshot.cache.battery_percentage;
    }
}

void telemetry_pipeline_get_metrics(const telemetry_pipeline_t *pipeline,
                                    telemetry_pipeline_metrics_t *out_metrics)
{
    if (pipeline == NULL || out_metrics == NULL) {
        return;
    }

    *out_metrics = pipeline->metrics;
}
