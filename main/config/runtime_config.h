#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "services/core/services.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Apply runtime JSON overrides to the supplied services configuration.
 *
 * The overrides are loaded from config/runtime/services.json (embedded in the
 * firmware image). Missing or blank files are treated as no-ops.
 *
 * @param config Pointer to the services configuration that should be mutated.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for bad input, or a parser
 *         error from cJSON if the file exists but is invalid.
 */
esp_err_t runtime_config_apply_services_overrides(services_config_t *config);

typedef struct {
	char services_sha256[65];
	char api_keys_sha256[65];
	uint32_t services_size_bytes;
	uint32_t api_keys_size_bytes;
} runtime_config_digest_t;

const runtime_config_digest_t *runtime_config_get_digest(void);

#ifdef __cplusplus
}
#endif
