#ifndef LOG_HISTORY_H
#define LOG_HISTORY_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOG_HISTORY_CATEGORY_INFO = 0,
    LOG_HISTORY_CATEGORY_WARNING = 1,
    LOG_HISTORY_CATEGORY_ERROR = 2,
    LOG_HISTORY_CATEGORY_SENSOR = 3,
} log_history_category_t;

#define LOG_HISTORY_FILTER_INFO    (1U << LOG_HISTORY_CATEGORY_INFO)
#define LOG_HISTORY_FILTER_WARNING (1U << LOG_HISTORY_CATEGORY_WARNING)
#define LOG_HISTORY_FILTER_ERROR   (1U << LOG_HISTORY_CATEGORY_ERROR)
#define LOG_HISTORY_FILTER_SENSOR  (1U << LOG_HISTORY_CATEGORY_SENSOR)
#define LOG_HISTORY_FILTER_ALL     (LOG_HISTORY_FILTER_INFO | \
                                    LOG_HISTORY_FILTER_WARNING | \
                                    LOG_HISTORY_FILTER_ERROR | \
                                    LOG_HISTORY_FILTER_SENSOR)

typedef struct {
    uint32_t sequence;
    int64_t timestamp_us;
    log_history_category_t category;
    char tag[24];
    char message[256];
} log_history_entry_t;

esp_err_t log_history_init(void);
void log_history_add_entry(log_history_category_t category, const char *tag, const char *message);
size_t log_history_get_entries(uint32_t filter_mask, size_t max_entries, log_history_entry_t *out_entries);
void log_history_record_sensor_json(const char *json_line);

#ifdef __cplusplus
}
#endif

#endif // LOG_HISTORY_H
