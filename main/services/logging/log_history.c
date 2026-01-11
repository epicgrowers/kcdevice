#include "services/logging/log_history.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define LOG_HISTORY_LINE_BUFFER_LEN 384

static const char *TAG = "LOG_HISTORY";
static SemaphoreHandle_t s_log_mutex;
static log_history_entry_t *s_entries;
static size_t s_write_index;
static size_t s_entry_count;
static uint32_t s_sequence_counter;
static bool s_initialized;
static vprintf_like_t s_prev_logger;

static void log_history_store(log_history_category_t category, const char *tag, const char *message);
static void log_history_ingest_line(const char *line);
static log_history_category_t log_history_category_from_line(const char *line);
static void log_history_extract_tag(const char *line, char *tag_buf, size_t tag_len, const char **message_start);
static int log_history_vprintf(const char *fmt, va_list args);

esp_err_t log_history_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_log_mutex = xSemaphoreCreateMutex();
    if (s_log_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_entries = heap_caps_calloc(LOG_HISTORY_STORAGE_DEPTH,
                                 sizeof(log_history_entry_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_entries == NULL) {
        ESP_LOGE(TAG, "Failed to allocate log history buffer");
        vSemaphoreDelete(s_log_mutex);
        s_log_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_prev_logger = esp_log_set_vprintf(log_history_vprintf);
    s_initialized = true;
    return ESP_OK;
}

void log_history_add_entry(log_history_category_t category, const char *tag, const char *message)
{
    if (!s_initialized || message == NULL) {
        return;
    }
    log_history_store(category, tag, message);
}

size_t log_history_get_entries(uint32_t filter_mask, size_t max_entries, log_history_entry_t *out_entries)
{
    if (!s_initialized || out_entries == NULL || max_entries == 0) {
        return 0;
    }

    if (xSemaphoreTake(s_log_mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }

    const size_t available = s_entry_count;
    size_t copied = 0;
    for (size_t i = 0; i < available && copied < max_entries; ++i) {
        const size_t index = (s_write_index + LOG_HISTORY_STORAGE_DEPTH - 1 - i) % LOG_HISTORY_STORAGE_DEPTH;
        const log_history_entry_t *entry = &s_entries[index];
        const uint32_t bit = 1U << entry->category;
        if ((bit & filter_mask) == 0) {
            continue;
        }
        out_entries[copied++] = *entry;
    }

    xSemaphoreGive(s_log_mutex);
    return copied;
}

void log_history_record_sensor_json(const char *json_line)
{
    if (json_line == NULL) {
        return;
    }
    log_history_add_entry(LOG_HISTORY_CATEGORY_SENSOR, "SENSOR", json_line);
}

static int log_history_vprintf(const char *fmt, va_list args)
{
    if (!s_initialized || fmt == NULL) {
        return (s_prev_logger != NULL) ? s_prev_logger(fmt, args) : vprintf(fmt, args);
    }

    char line_buffer[LOG_HISTORY_LINE_BUFFER_LEN];
    va_list args_copy;
    va_copy(args_copy, args);
    vsnprintf(line_buffer, sizeof(line_buffer), fmt, args_copy);
    va_end(args_copy);

    log_history_ingest_line(line_buffer);

    if (s_prev_logger != NULL) {
        return s_prev_logger(fmt, args);
    }
    return vprintf(fmt, args);
}

static void log_history_ingest_line(const char *line)
{
    if (line == NULL || line[0] == '\0') {
        return;
    }

    log_history_category_t category = log_history_category_from_line(line);
    char tag[sizeof(((log_history_entry_t *)0)->tag)] = {0};
    const char *message = line;
    log_history_extract_tag(line, tag, sizeof(tag), &message);
    log_history_store(category, tag, message);
}

static log_history_category_t log_history_category_from_line(const char *line)
{
    if (line == NULL) {
        return LOG_HISTORY_CATEGORY_INFO;
    }

    const char *ptr = line;
    while (*ptr != '\0') {
        unsigned char ch = (unsigned char)*ptr;

        if (ch == '\x1b') {
            ++ptr;
            if (*ptr == '[') {
                ++ptr;
                while (*ptr != '\0' && *ptr != 'm') {
                    ++ptr;
                }
                if (*ptr == 'm') {
                    ++ptr;
                }
                continue;
            }
        }

        if (isspace(ch)) {
            ++ptr;
            continue;
        }

        ch = (unsigned char)toupper(ch);
        switch (ch) {
            case 'E':
                return LOG_HISTORY_CATEGORY_ERROR;
            case 'W':
                return LOG_HISTORY_CATEGORY_WARNING;
            case 'I':
            case 'D':
            case 'V':
                return LOG_HISTORY_CATEGORY_INFO;
            default:
                break;
        }

        ++ptr;
    }

    return LOG_HISTORY_CATEGORY_INFO;
}

static void log_history_extract_tag(const char *line, char *tag_buf, size_t tag_len, const char **message_start)
{
    if (tag_buf == NULL || tag_len == 0 || message_start == NULL) {
        return;
    }

    const char *paren = strchr(line, ')');
    if (paren != NULL && paren[1] == ' ') {
        const char *tag_start = paren + 2;
        const char *colon = strchr(tag_start, ':');
        if (colon != NULL) {
            size_t len = (size_t)(colon - tag_start);
            if (len >= tag_len) {
                len = tag_len - 1;
            }
            memcpy(tag_buf, tag_start, len);
            tag_buf[len] = '\0';
            *message_start = colon + 1;
            while (**message_start == ' ') {
                (*message_start)++;
            }
            return;
        }
    }

    strlcpy(tag_buf, "LOG", tag_len);
    *message_start = line;
}

static void log_history_store(log_history_category_t category, const char *tag, const char *message)
{
    if (s_log_mutex == NULL || message == NULL) {
        return;
    }

    log_history_entry_t temp = {0};
    temp.category = category;
    temp.timestamp_us = esp_timer_get_time();
    if (tag != NULL) {
        strlcpy(temp.tag, tag, sizeof(temp.tag));
    } else {
        strlcpy(temp.tag, "LOG", sizeof(temp.tag));
    }
    strlcpy(temp.message, message, sizeof(temp.message));

    if (xSemaphoreTake(s_log_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    temp.sequence = ++s_sequence_counter;
    s_entries[s_write_index] = temp;
    s_write_index = (s_write_index + 1) % LOG_HISTORY_STORAGE_DEPTH;
    if (s_entry_count < LOG_HISTORY_STORAGE_DEPTH) {
        ++s_entry_count;
    }

    xSemaphoreGive(s_log_mutex);
}
