#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t check_interval_ms;
    uint32_t failure_threshold;
    const char *probe_hostname;
} connectivity_watchdog_config_t;

esp_err_t connectivity_watchdog_start(const connectivity_watchdog_config_t *config);
esp_err_t connectivity_watchdog_stop(void);

#ifdef __cplusplus
}
#endif
