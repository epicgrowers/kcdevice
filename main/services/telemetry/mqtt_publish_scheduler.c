#include "services/telemetry/mqtt_publish_scheduler.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define MQTT_SCHED_DEFAULT_IDLE_MS        200U
#define MQTT_SCHED_OFFLINE_BACKOFF_MS    1000U

static const char *TAG = "MQTT_SCHED";

static inline int64_t ms_to_us(uint32_t ms)
{
    return (int64_t)ms * 1000LL;
}

static inline uint32_t us_to_ms(int64_t us)
{
    if (us <= 0) {
        return MQTT_SCHED_DEFAULT_IDLE_MS;
    }
    return (uint32_t)(us / 1000LL);
}

void mqtt_publish_scheduler_init(mqtt_publish_scheduler_t *scheduler, uint32_t interval_sec)
{
    if (scheduler == NULL) {
        return;
    }

    scheduler->interval_sec = interval_sec;
    scheduler->last_publish_us = 0;
    scheduler->next_retry_us = 0;
    scheduler->manual_request = false;
}

void mqtt_publish_scheduler_set_interval(mqtt_publish_scheduler_t *scheduler, uint32_t interval_sec)
{
    if (scheduler == NULL) {
        return;
    }
    scheduler->interval_sec = interval_sec;
}

void mqtt_publish_scheduler_request_publish(mqtt_publish_scheduler_t *scheduler)
{
    if (scheduler == NULL) {
        return;
    }
    scheduler->manual_request = true;
}

void mqtt_publish_scheduler_note_publish(mqtt_publish_scheduler_t *scheduler, int64_t now_us)
{
    if (scheduler == NULL) {
        return;
    }
    scheduler->last_publish_us = now_us;
    scheduler->next_retry_us = 0;
    scheduler->manual_request = false;
}

void mqtt_publish_scheduler_delay(mqtt_publish_scheduler_t *scheduler, uint32_t delay_ms, int64_t now_us)
{
    if (scheduler == NULL) {
        return;
    }
    int64_t candidate = now_us + ms_to_us(delay_ms);
    if (candidate > scheduler->next_retry_us) {
        scheduler->next_retry_us = candidate;
    }
}

static uint32_t compute_wait_ms(const mqtt_publish_scheduler_t *scheduler, int64_t now_us, int64_t target_us)
{
    int64_t effective_target = target_us;
    if (scheduler->next_retry_us > now_us && scheduler->next_retry_us > effective_target) {
        effective_target = scheduler->next_retry_us;
    }

    if (effective_target <= now_us) {
        return MQTT_SCHED_DEFAULT_IDLE_MS;
    }

    return us_to_ms(effective_target - now_us);
}

mqtt_publish_scheduler_decision_t mqtt_publish_scheduler_next_action(mqtt_publish_scheduler_t *scheduler,
                                                                     const mqtt_publish_scheduler_inputs_t *inputs,
                                                                     int64_t now_us)
{
    mqtt_publish_scheduler_decision_t decision = {
        .should_publish = false,
        .should_block = false,
        .wait_ms = MQTT_SCHED_DEFAULT_IDLE_MS,
    };

    if (scheduler == NULL || inputs == NULL) {
        return decision;
    }

    if (!inputs->mqtt_connected) {
        mqtt_publish_scheduler_delay(scheduler, MQTT_SCHED_OFFLINE_BACKOFF_MS, now_us);
        decision.wait_ms = compute_wait_ms(scheduler, now_us, now_us + ms_to_us(MQTT_SCHED_OFFLINE_BACKOFF_MS));
        return decision;
    }

    bool interval_enabled = (scheduler->interval_sec > 0);
    int64_t interval_us = interval_enabled ? ms_to_us(scheduler->interval_sec * 1000U) : 0;

    if (!interval_enabled && !scheduler->manual_request) {
        decision.should_block = true;
        decision.wait_ms = 0;
        return decision;
    }

    bool due = scheduler->manual_request;
    int64_t next_due_us = now_us;

    if (!due && interval_enabled) {
        if (scheduler->last_publish_us == 0) {
            due = true;
        } else {
            next_due_us = scheduler->last_publish_us + interval_us;
            if (now_us >= next_due_us) {
                due = true;
            }
        }
    }

    if (!due) {
        decision.wait_ms = compute_wait_ms(scheduler, now_us, next_due_us);
        return decision;
    }

    if (scheduler->next_retry_us > now_us && !scheduler->manual_request) {
        decision.wait_ms = us_to_ms(scheduler->next_retry_us - now_us);
        return decision;
    }

    scheduler->manual_request = false;
    decision.should_publish = true;
    decision.wait_ms = 0;
    return decision;
}
