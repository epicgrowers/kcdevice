#pragma once

#include "esp_err.h"
#include "sensors/pipeline.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *device_id;
    const char *device_name;
} mqtt_payload_context_t;

/**
 * @brief Build an MQTT telemetry payload from a sensor snapshot.
 *
 * @param ctx Payload context providing device metadata.
 * @param snapshot Sensor snapshot retrieved via sensor_pipeline_snapshot().
 * @param out_json On success, receives a heap-allocated JSON string (caller frees).
 * @return ESP_OK on success, or an error code if inputs are invalid or allocation failed.
 */
esp_err_t mqtt_payload_build(const mqtt_payload_context_t *ctx,
                             const sensor_pipeline_snapshot_t *snapshot,
                             char **out_json);

#ifdef __cplusplus
}
#endif
