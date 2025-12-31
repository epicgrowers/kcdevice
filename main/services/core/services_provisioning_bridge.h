#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "services/core/services.h"

#ifdef __cplusplus
extern "C" {
#endif

void services_provisioning_bridge_init(const services_provisioning_api_t *provisioning);

void services_provisioning_bridge_deinit(void);

const services_provisioning_api_t *services_provisioning_bridge_get(void);

bool services_provisioning_bridge_has_device_identity(void);

bool services_provisioning_bridge_has_https_credentials(void);

bool services_provisioning_bridge_has_mqtt_ca(void);

esp_err_t services_provisioning_load_device_id(char *buffer, size_t length);

esp_err_t services_provisioning_load_device_name(char *buffer, size_t length);

esp_err_t services_provisioning_set_device_name(const char *value);

esp_err_t services_provisioning_clear_device_name(void);

esp_err_t services_provisioning_load_https_certificate(char *buffer, size_t *length);

esp_err_t services_provisioning_load_https_private_key(char *buffer, size_t *length);

esp_err_t services_provisioning_load_mqtt_ca_certificate(char *buffer, size_t *length);

#ifdef __cplusplus
}
#endif
