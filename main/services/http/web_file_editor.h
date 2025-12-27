/**
 * @file web_file_editor.h
 * @brief Web file editor API for FATFS-based dashboard customization
 */

#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_EDITOR_FS_PATH "/www"
#define WEB_EDITOR_MAX_FILE_SIZE (200 * 1024)  // 200KB

/**
 * @brief Initialize FATFS file system and copy embedded files on first boot
 * @return ESP_OK on success
 */
esp_err_t web_editor_init_fs(void);

/**
 * @brief Load a file from FATFS
 * @param filename Name of the file (without path)
 * @param content Output buffer pointer (will be allocated, must be freed by caller)
 * @param size Output size of the loaded content
 * @return ESP_OK on success
 */
esp_err_t web_editor_load_file(const char *filename, char **content, size_t *size);

/**
 * @brief Save a file to FATFS
 * @param filename Name of the file (without path)
 * @param content File content to save
 * @param size Size of the content
 * @return ESP_OK on success
 */
esp_err_t web_editor_save_file(const char *filename, const char *content, size_t size);

/**
 * @brief List all files in FATFS
 * @param json_output Output JSON string (must be freed by caller)
 * @return ESP_OK on success
 */
esp_err_t web_editor_list_files(char **json_output);

/**
 * @brief Get content type for a filename
 * @param filename Name of the file
 * @return MIME type string
 */
const char* web_editor_get_content_type(const char *filename);

/**
 * @brief Format the FATFS volume and restore embedded default dashboard files
 * @return ESP_OK on success
 */
esp_err_t web_editor_reset_fs(void);

#ifdef __cplusplus
}
#endif
