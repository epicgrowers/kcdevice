#include "services/telemetry/mqtt_payload_builder.h"

#include <string.h>
#include "cJSON.h"

static void add_sensor_payload(cJSON *sensors_object, const cached_sensor_t *sensor)
{
    if (!sensor || !sensor->valid || sensors_object == NULL) {
        return;
    }

    if (sensor->value_count == 1) {
        cJSON_AddNumberToObject(sensors_object, sensor->sensor_type, sensor->values[0]);
        return;
    }

    cJSON *sensor_obj = cJSON_CreateObject();
    if (sensor_obj == NULL) {
        return;
    }

    if (strcmp(sensor->sensor_type, "HUM") == 0) {
        if (sensor->value_count >= 1) cJSON_AddNumberToObject(sensor_obj, "humidity", sensor->values[0]);
        if (sensor->value_count >= 2) cJSON_AddNumberToObject(sensor_obj, "air_temp", sensor->values[1]);
        if (sensor->value_count >= 3) cJSON_AddNumberToObject(sensor_obj, "dew_point", sensor->values[2]);
    } else if (strcmp(sensor->sensor_type, "EC") == 0) {
        if (sensor->value_count >= 1) cJSON_AddNumberToObject(sensor_obj, "conductivity", sensor->values[0]);
        if (sensor->value_count >= 2) cJSON_AddNumberToObject(sensor_obj, "tds", sensor->values[1]);
        if (sensor->value_count >= 3) cJSON_AddNumberToObject(sensor_obj, "salinity", sensor->values[2]);
        if (sensor->value_count >= 4) cJSON_AddNumberToObject(sensor_obj, "specific_gravity", sensor->values[3]);
    } else if (strcmp(sensor->sensor_type, "DO") == 0) {
        if (sensor->value_count >= 1) cJSON_AddNumberToObject(sensor_obj, "dissolved_oxygen", sensor->values[0]);
        if (sensor->value_count >= 2) cJSON_AddNumberToObject(sensor_obj, "saturation", sensor->values[1]);
    } else {
        for (uint8_t i = 0; i < sensor->value_count; ++i) {
            char key[8];
            snprintf(key, sizeof(key), "v%u", i);
            cJSON_AddNumberToObject(sensor_obj, key, sensor->values[i]);
        }
    }

    cJSON_AddItemToObject(sensors_object, sensor->sensor_type, sensor_obj);
}

esp_err_t mqtt_payload_build(const mqtt_payload_context_t *ctx,
                             const sensor_pipeline_snapshot_t *snapshot,
                             char **out_json)
{
    if (ctx == NULL || snapshot == NULL || out_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!snapshot->cache_valid ) {
        return ESP_ERR_INVALID_STATE;
    }

    const sensor_cache_t *cache = &snapshot->cache;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "device_id", ctx->device_id ? ctx->device_id : "unknown");
    if (ctx->device_name != NULL && ctx->device_name[0] != '\0') {
        cJSON_AddStringToObject(root, "device_name", ctx->device_name);
    }

    cJSON *sensors = cJSON_CreateObject();
    if (sensors == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    for (uint8_t i = 0; i < cache->sensor_count && i < 8; i++) {
        add_sensor_payload(sensors, &cache->sensors[i]);
    }
    cJSON_AddItemToObject(root, "sensors", sensors);
    cJSON_AddBoolToObject(root, "sensors_ready", snapshot->readiness.sensors_ready);
    cJSON_AddNumberToObject(root, "sensor_interval_sec", snapshot->reading_interval_sec);

    if (cache->battery_valid) {
        cJSON_AddNumberToObject(root, "battery", cache->battery_percentage);
    }

    cJSON_AddNumberToObject(root, "rssi", cache->rssi);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *out_json = json_str;
    return ESP_OK;
}
