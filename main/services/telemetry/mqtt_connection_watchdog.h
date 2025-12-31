#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t poll_interval_ms;
    uint32_t stuck_threshold_ms;
    uint32_t report_interval_ms;
} mqtt_connection_watchdog_config_t;

typedef struct {
    bool active;
    bool tripped;
    uint32_t trip_count;
    int64_t unhealthy_since_us;
    int64_t last_report_us;
    uint32_t poll_interval_ms;
    uint32_t stuck_threshold_ms;
    uint32_t report_interval_ms;
} mqtt_watchdog_metrics_t;

esp_err_t mqtt_connection_watchdog_start(const mqtt_connection_watchdog_config_t *config);
void mqtt_connection_watchdog_stop(void);
void mqtt_connection_watchdog_get_metrics(mqtt_watchdog_metrics_t *metrics);

#ifdef __cplusplus
}
#endif
