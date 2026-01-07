#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the SD card logger and start its background task.
 *
 * @return ESP_OK when the task launches (logging may still wait for SD insertion),
 *         ESP_ERR_NOT_SUPPORTED when disabled, or an error code otherwise.
 */
esp_err_t sd_logger_init(void);

/**
 * @brief Stop the SD card logger task and unmount the card if it is mounted.
 */
void sd_logger_shutdown(void);

#ifdef __cplusplus
}
#endif
