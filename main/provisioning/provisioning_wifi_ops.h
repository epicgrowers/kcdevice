#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct provisioning_wifi_ops_s {
    esp_err_t (*init)(void);
    esp_err_t (*connect)(const char *ssid, const char *password);
    bool (*is_connected)(void);
    esp_err_t (*get_stored_credentials)(char *ssid, char *password);
    esp_err_t (*disconnect)(void);
    esp_err_t (*clear_credentials)(void);
} provisioning_wifi_ops_t;

static inline bool provisioning_wifi_ops_is_valid(const provisioning_wifi_ops_t *ops)
{
    return (ops != NULL &&
            ops->init != NULL &&
            ops->connect != NULL &&
            ops->is_connected != NULL &&
            ops->get_stored_credentials != NULL &&
            ops->disconnect != NULL &&
            ops->clear_credentials != NULL);
}

const provisioning_wifi_ops_t *provisioning_wifi_ops_default(void);

#ifdef __cplusplus
}
#endif
