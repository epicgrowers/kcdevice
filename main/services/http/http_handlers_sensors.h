#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "sensors/sensor_manager.h"
#include "sensors/drivers/ezo_sensor.h"
#include "cJSON.h"

typedef struct {
	bool should_resume;
	bool mutex_acquired;
} sensor_read_guard_t;

void sensor_read_guard_acquire(sensor_read_guard_t *guard);
void sensor_read_guard_release(sensor_read_guard_t *guard);
bool parse_sensor_address_from_uri(const char *uri, const char *prefix, uint8_t *address);
ezo_sensor_t *find_sensor_by_address(uint8_t address);
void add_capabilities_array(cJSON *parent, uint32_t flags);
void append_sensor_runtime_info(cJSON *json, ezo_sensor_t *sensor);
cJSON *build_sensor_json(ezo_sensor_t *sensor, int index, bool include_runtime);
cJSON *parse_request_json_body(httpd_req_t *req, char **raw_buffer);
void add_sample_readings_to_json(cJSON *json, const char *type, const float values[], uint8_t count);
esp_err_t send_sensor_success_response(httpd_req_t *req, ezo_sensor_t *sensor);

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t http_handlers_sensors_init(httpd_handle_t server);
void http_handlers_sensors_deinit(void);

#ifdef __cplusplus
}
#endif
