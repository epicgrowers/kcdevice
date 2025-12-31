#include "services/telemetry/mqtt_publish_loop.h"

#include <string.h>

#define MQTT_PUBLISH_LOOP_DEFAULT_BUSY_BACKOFF_MS    1000U
#define MQTT_PUBLISH_LOOP_DEFAULT_CLIENT_BACKOFF_MS   500U
#define MQTT_PUBLISH_LOOP_DEFAULT_IDLE_DELAY_MS       200U

static uint32_t sanitize_delay(uint32_t delay_ms, uint32_t fallback_ms)
{
    return (delay_ms > 0) ? delay_ms : fallback_ms;
}

static void fill_default_config(mqtt_publish_loop_config_t *config)
{
    if (config == NULL) {
        return;
    }

    if (config->busy_backoff_ms == 0) {
        config->busy_backoff_ms = MQTT_PUBLISH_LOOP_DEFAULT_BUSY_BACKOFF_MS;
    }
    if (config->client_backoff_ms == 0) {
        config->client_backoff_ms = MQTT_PUBLISH_LOOP_DEFAULT_CLIENT_BACKOFF_MS;
    }
    if (config->idle_delay_ms == 0) {
        config->idle_delay_ms = MQTT_PUBLISH_LOOP_DEFAULT_IDLE_DELAY_MS;
    }
}

void mqtt_publish_loop_init(mqtt_publish_loop_t *loop,
                            mqtt_publish_scheduler_t *scheduler,
                            const mqtt_publish_loop_config_t *config)
{
    if (loop == NULL) {
        return;
    }

    memset(loop, 0, sizeof(*loop));
    loop->scheduler = scheduler;

    mqtt_publish_loop_config_t resolved = {0};
    if (config != NULL) {
        resolved = *config;
    }
    fill_default_config(&resolved);
    loop->config = resolved;
}

mqtt_publish_loop_decision_t mqtt_publish_loop_next_action(mqtt_publish_loop_t *loop,
                                                           const mqtt_publish_loop_inputs_t *inputs,
                                                           int64_t now_us)
{
    mqtt_publish_loop_decision_t decision = {
        .action = MQTT_PUBLISH_LOOP_ACTION_WAIT,
        .wait_ms = MQTT_PUBLISH_LOOP_DEFAULT_IDLE_DELAY_MS,
    };

    if (loop == NULL || loop->scheduler == NULL || inputs == NULL) {
        return decision;
    }

    mqtt_publish_scheduler_inputs_t scheduler_inputs = {
        .mqtt_connected = inputs->mqtt_connected,
    };

    mqtt_publish_scheduler_decision_t scheduler_decision =
        mqtt_publish_scheduler_next_action(loop->scheduler, &scheduler_inputs, now_us);

    if (!scheduler_decision.should_publish) {
        if (scheduler_decision.should_block) {
            decision.action = MQTT_PUBLISH_LOOP_ACTION_BLOCK;
            decision.wait_ms = 0;
            return decision;
        }

        decision.action = MQTT_PUBLISH_LOOP_ACTION_WAIT;
        decision.wait_ms = sanitize_delay(scheduler_decision.wait_ms, loop->config.idle_delay_ms);
        return decision;
    }

    if (inputs->sensor_reading_in_progress || inputs->sensor_reading_paused) {
        mqtt_publish_scheduler_delay(loop->scheduler,
                                     loop->config.busy_backoff_ms,
                                     now_us);
        decision.action = MQTT_PUBLISH_LOOP_ACTION_WAIT;
        decision.wait_ms = loop->config.busy_backoff_ms;
        return decision;
    }

    decision.action = MQTT_PUBLISH_LOOP_ACTION_PUBLISH;
    decision.wait_ms = 0;
    return decision;
}

void mqtt_publish_loop_handle_result(mqtt_publish_loop_t *loop,
                                     mqtt_publish_loop_result_t result,
                                     int64_t now_us)
{
    if (loop == NULL || loop->scheduler == NULL) {
        return;
    }

    switch (result) {
        case MQTT_PUBLISH_RESULT_SUCCESS:
            mqtt_publish_scheduler_note_publish(loop->scheduler, now_us);
            break;
        case MQTT_PUBLISH_RESULT_SENSOR_BUSY:
        case MQTT_PUBLISH_RESULT_SNAPSHOT_UNAVAILABLE:
        case MQTT_PUBLISH_RESULT_PAYLOAD_ERROR:
        case MQTT_PUBLISH_RESULT_TEMPORARY_FAILURE:
            mqtt_publish_scheduler_delay(loop->scheduler,
                                         loop->config.busy_backoff_ms,
                                         now_us);
            break;
        case MQTT_PUBLISH_RESULT_CLIENT_UNAVAILABLE:
            mqtt_publish_scheduler_delay(loop->scheduler,
                                         loop->config.client_backoff_ms,
                                         now_us);
            break;
        default:
            break;
    }
}

uint32_t mqtt_publish_loop_get_idle_delay(const mqtt_publish_loop_t *loop)
{
    if (loop == NULL) {
        return MQTT_PUBLISH_LOOP_DEFAULT_IDLE_DELAY_MS;
    }
    return sanitize_delay(loop->config.idle_delay_ms, MQTT_PUBLISH_LOOP_DEFAULT_IDLE_DELAY_MS);
}
