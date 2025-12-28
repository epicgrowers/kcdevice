#pragma once

#include "services/core/services.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Populate a services_config_t struct with project defaults.
 *
 * The caller may override any fields (e.g., TLS readiness or callbacks)
 * before passing the config to services_start().
 */
void services_config_load_defaults(services_config_t *config);

#ifdef __cplusplus
}
#endif
