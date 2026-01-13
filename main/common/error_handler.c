/**
 * @file error_handler.c
 * @brief Implementation of unified error handling framework
 */

#include "common/error_handler.h"
#include "esp_log.h"
#include <string.h>

// Extract filename from full path for cleaner logs
static const char* extract_filename(const char *path)
{
    if (path == NULL) {
        return "unknown";
    }

    // Find last path separator (works for both / and \)
    const char *filename = strrchr(path, '/');
    if (filename == NULL) {
        filename = strrchr(path, '\\');
    }

    return (filename != NULL) ? (filename + 1) : path;
}

void error_log_and_return(esp_err_t err,
                          const char *message,
                          const char *file,
                          int line,
                          const char *function,
                          const char *tag)
{
    const char *filename = extract_filename(file);
    const char *err_name = esp_err_to_name(err);

    ESP_LOGE(tag,
             "%s [%s:%d in %s()] - Error: %s (0x%x)",
             message,
             filename,
             line,
             function,
             err_name,
             err);
}

void error_log_warning(esp_err_t err,
                      const char *message,
                      const char *file,
                      int line,
                      const char *function,
                      const char *tag)
{
    const char *filename = extract_filename(file);
    const char *err_name = esp_err_to_name(err);

    ESP_LOGW(tag,
             "%s [%s:%d in %s()] - Warning: %s (0x%x)",
             message,
             filename,
             line,
             function,
             err_name,
             err);
}

void error_log_fatal(esp_err_t err,
                    const char *message,
                    const char *file,
                    int line,
                    const char *function,
                    const char *tag)
{
    const char *filename = extract_filename(file);
    const char *err_name = esp_err_to_name(err);

    ESP_LOGE(tag,
             "FATAL: %s [%s:%d in %s()] - Error: %s (0x%x)",
             message,
             filename,
             line,
             function,
             err_name,
             err);

    ESP_LOGE(tag, "System cannot continue - aborting");
    abort();
}
