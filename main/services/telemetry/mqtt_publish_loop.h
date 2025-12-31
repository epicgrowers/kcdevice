#pragma once

#include "services/telemetry/mqtt_publish_scheduler.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t busy_backoff_ms;
    uint32_t client_backoff_ms;
    uint32_t idle_delay_ms;
} mqtt_publish_loop_config_t;

typedef struct {
    bool mqtt_connected;
    bool sensor_reading_in_progress;
    bool sensor_reading_paused;
} mqtt_publish_loop_inputs_t;

typedef enum {
    MQTT_PUBLISH_LOOP_ACTION_WAIT = 0,
    MQTT_PUBLISH_LOOP_ACTION_BLOCK = 1,
    MQTT_PUBLISH_LOOP_ACTION_PUBLISH = 2,
} mqtt_publish_loop_action_t;

typedef struct {
    mqtt_publish_loop_action_t action;
    uint32_t wait_ms;
} mqtt_publish_loop_decision_t;

typedef enum {
    MQTT_PUBLISH_RESULT_SUCCESS = 0,
    MQTT_PUBLISH_RESULT_SENSOR_BUSY,
    MQTT_PUBLISH_RESULT_SNAPSHOT_UNAVAILABLE,
    MQTT_PUBLISH_RESULT_PAYLOAD_ERROR,
    MQTT_PUBLISH_RESULT_CLIENT_UNAVAILABLE,
    MQTT_PUBLISH_RESULT_TEMPORARY_FAILURE,
} mqtt_publish_loop_result_t;

typedef struct {
    mqtt_publish_scheduler_t *scheduler;
    mqtt_publish_loop_config_t config;
} mqtt_publish_loop_t;

void mqtt_publish_loop_init(mqtt_publish_loop_t *loop,
                            mqtt_publish_scheduler_t *scheduler,
                            const mqtt_publish_loop_config_t *config);

mqtt_publish_loop_decision_t mqtt_publish_loop_next_action(mqtt_publish_loop_t *loop,
                                                           const mqtt_publish_loop_inputs_t *inputs,
                                                           int64_t now_us);

void mqtt_publish_loop_handle_result(mqtt_publish_loop_t *loop,
                                     mqtt_publish_loop_result_t result,
                                     int64_t now_us);

uint32_t mqtt_publish_loop_get_idle_delay(const mqtt_publish_loop_t *loop);

#ifdef __cplusplus
}
#endif
