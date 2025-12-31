#include "services/core/services_provisioning_bridge.h"

#include <string.h>

static const services_provisioning_api_t *s_provisioning = NULL;

static esp_err_t validate_string_buffer(char *buffer, size_t length)
{
    if (buffer == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    buffer[0] = '\0';
    return ESP_OK;
}

static esp_err_t validate_blob_args(char *buffer, size_t *length)
{
    if (buffer == NULL || length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

void services_provisioning_bridge_init(const services_provisioning_api_t *provisioning)
{
    s_provisioning = provisioning;
}

void services_provisioning_bridge_deinit(void)
{
    s_provisioning = NULL;
}

const services_provisioning_api_t *services_provisioning_bridge_get(void)
{
    return s_provisioning;
}

bool services_provisioning_bridge_has_device_identity(void)
{
    return s_provisioning != NULL &&
           s_provisioning->load_device_id != NULL;
}

bool services_provisioning_bridge_has_https_credentials(void)
{
    return s_provisioning != NULL &&
           s_provisioning->load_https_certificate != NULL &&
           s_provisioning->load_https_private_key != NULL;
}

bool services_provisioning_bridge_has_mqtt_ca(void)
{
    return s_provisioning != NULL &&
           s_provisioning->load_mqtt_ca_certificate != NULL;
}

esp_err_t services_provisioning_load_device_id(char *buffer, size_t length)
{
    esp_err_t arg_check = validate_string_buffer(buffer, length);
    if (arg_check != ESP_OK) {
        return arg_check;
    }

    if (s_provisioning == NULL || s_provisioning->load_device_id == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return s_provisioning->load_device_id(s_provisioning->ctx, buffer, length);
}

esp_err_t services_provisioning_load_device_name(char *buffer, size_t length)
{
    esp_err_t arg_check = validate_string_buffer(buffer, length);
    if (arg_check != ESP_OK) {
        return arg_check;
    }

    if (s_provisioning == NULL || s_provisioning->load_device_name == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return s_provisioning->load_device_name(s_provisioning->ctx, buffer, length);
}

esp_err_t services_provisioning_set_device_name(const char *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_provisioning == NULL || s_provisioning->set_device_name == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return s_provisioning->set_device_name(s_provisioning->ctx, value);
}

esp_err_t services_provisioning_clear_device_name(void)
{
    if (s_provisioning == NULL || s_provisioning->clear_device_name == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return s_provisioning->clear_device_name(s_provisioning->ctx);
}

esp_err_t services_provisioning_load_https_certificate(char *buffer, size_t *length)
{
    esp_err_t arg_check = validate_blob_args(buffer, length);
    if (arg_check != ESP_OK) {
        return arg_check;
    }

    if (s_provisioning == NULL || s_provisioning->load_https_certificate == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return s_provisioning->load_https_certificate(s_provisioning->ctx, buffer, length);
}

esp_err_t services_provisioning_load_https_private_key(char *buffer, size_t *length)
{
    esp_err_t arg_check = validate_blob_args(buffer, length);
    if (arg_check != ESP_OK) {
        return arg_check;
    }

    if (s_provisioning == NULL || s_provisioning->load_https_private_key == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return s_provisioning->load_https_private_key(s_provisioning->ctx, buffer, length);
}

esp_err_t services_provisioning_load_mqtt_ca_certificate(char *buffer, size_t *length)
{
    esp_err_t arg_check = validate_blob_args(buffer, length);
    if (arg_check != ESP_OK) {
        return arg_check;
    }

    if (s_provisioning == NULL || s_provisioning->load_mqtt_ca_certificate == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return s_provisioning->load_mqtt_ca_certificate(s_provisioning->ctx, buffer, length);
}
