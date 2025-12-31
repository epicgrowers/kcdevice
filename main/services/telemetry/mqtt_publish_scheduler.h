#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t interval_sec;
    int64_t last_publish_us;
    int64_t next_retry_us;
    bool manual_request;
} mqtt_publish_scheduler_t;

typedef struct {
    bool mqtt_connected;
} mqtt_publish_scheduler_inputs_t;

typedef struct {
    bool should_publish;
    bool should_block;
    uint32_t wait_ms;
} mqtt_publish_scheduler_decision_t;

void mqtt_publish_scheduler_init(mqtt_publish_scheduler_t *scheduler, uint32_t interval_sec);
void mqtt_publish_scheduler_set_interval(mqtt_publish_scheduler_t *scheduler, uint32_t interval_sec);
void mqtt_publish_scheduler_request_publish(mqtt_publish_scheduler_t *scheduler);
void mqtt_publish_scheduler_note_publish(mqtt_publish_scheduler_t *scheduler, int64_t now_us);
void mqtt_publish_scheduler_delay(mqtt_publish_scheduler_t *scheduler, uint32_t delay_ms, int64_t now_us);
mqtt_publish_scheduler_decision_t mqtt_publish_scheduler_next_action(mqtt_publish_scheduler_t *scheduler,
                                                                     const mqtt_publish_scheduler_inputs_t *inputs,
                                                                     int64_t now_us);

#ifdef __cplusplus
}
#endif
