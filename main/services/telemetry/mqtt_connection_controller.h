#pragma once

#include "esp_err.h"
#include "mqtt_client.h"
#include "services/telemetry/mqtt_telemetry.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	mqtt_state_t state;
	bool should_run;
	bool client_running;
	bool supervisor_running;
	uint32_t reconnect_count;
	uint32_t consecutive_failures;
	int64_t last_transition_us;
} mqtt_connection_metrics_t;

esp_err_t mqtt_connection_controller_set_client(esp_mqtt_client_handle_t client);
void mqtt_connection_controller_clear_client(void);
esp_mqtt_client_handle_t mqtt_connection_controller_get_client(void);

esp_err_t mqtt_connection_controller_start_supervisor(void);
esp_err_t mqtt_connection_controller_stop_supervisor(void);

void mqtt_connection_controller_request_run(bool should_run);
esp_err_t mqtt_connection_controller_stop_client(void);

mqtt_state_t mqtt_connection_controller_get_state(void);
bool mqtt_connection_controller_is_connected(void);
bool mqtt_connection_controller_is_client_running(void);
uint32_t mqtt_connection_controller_get_reconnects(void);
void mqtt_connection_controller_get_metrics(mqtt_connection_metrics_t *metrics);

void mqtt_connection_controller_handle_connected(void);
void mqtt_connection_controller_handle_disconnected(void);
void mqtt_connection_controller_handle_error(void);

#ifdef __cplusplus
}
#endif
