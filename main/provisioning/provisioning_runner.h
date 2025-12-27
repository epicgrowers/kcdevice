#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/**
 * @brief Callback fired when provisioning enters the BLE phase.
 *        Allows callers to launch long-running work (e.g., sensor boot) in parallel.
 */
typedef void (*provisioning_parallel_cb_t)(void *ctx);

typedef struct {
    EventGroupHandle_t sensor_event_group;
    EventBits_t sensors_ready_bit;
    uint32_t sensor_log_interval_ms;
    provisioning_parallel_cb_t on_parallel_work_start;
    void *on_parallel_work_ctx;
    bool attempt_stored_credentials_first;
} provisioning_plan_t;

typedef struct {
    bool connected;
    bool used_stored_credentials;
    bool provisioning_triggered;
    bool sensors_ready_during_provisioning;
} provisioning_outcome_t;

esp_err_t provisioning_run(const provisioning_plan_t *plan, provisioning_outcome_t *outcome);

void provisioning_connection_guard_poll(void);

typedef struct {
    bool has_credentials;
    char ssid[33];
} provisioning_saved_network_info_t;

esp_err_t provisioning_get_saved_network(provisioning_saved_network_info_t *info);

esp_err_t provisioning_clear_saved_network(void);

bool provisioning_wifi_is_connected(void);

esp_err_t provisioning_disconnect(void);
