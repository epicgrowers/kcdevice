/**
 * @file secure_logging.c
 * @brief Implementation of secure logging utilities
 */

#include "common/secure_logging.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

// Thread-local storage for sanitized string (not truly thread-safe but good enough for logging)
static char s_sanitized_buffer[SECURE_LOG_MAX_LEN];

const char* secure_sanitize_string(const char *str, bool is_password)
{
    if (str == NULL) {
        return "<null>";
    }

    size_t len = strlen(str);

    if (len == 0) {
        return "<empty>";
    }

    if (is_password) {
        // Completely mask passwords
        snprintf(s_sanitized_buffer, sizeof(s_sanitized_buffer),
                 "******* (%zu chars)", len);
    } else {
        // Show first 2 chars, mask the rest
        if (len <= 2) {
            snprintf(s_sanitized_buffer, sizeof(s_sanitized_buffer),
                     "*** (%zu chars)", len);
        } else {
            snprintf(s_sanitized_buffer, sizeof(s_sanitized_buffer),
                     "%c%c*** (%zu chars)", str[0], str[1], len);
        }
    }

    return s_sanitized_buffer;
}

const char* secure_mask_password(const char *password)
{
    return secure_sanitize_string(password, true);
}

const char* secure_mask_ssid(const char *ssid)
{
    return secure_sanitize_string(ssid, false);
}

const char* secure_mask_api_key(const char *api_key)
{
    if (api_key == NULL) {
        return "<null>";
    }

    size_t len = strlen(api_key);

    if (len == 0) {
        return "<empty>";
    }

    if (len <= 8) {
        // Too short, just show length
        snprintf(s_sanitized_buffer, sizeof(s_sanitized_buffer),
                 "***...*** (%zu chars)", len);
    } else {
        // Show first 4 and last 4 characters
        snprintf(s_sanitized_buffer, sizeof(s_sanitized_buffer),
                 "%.4s...%.4s", api_key, api_key + len - 4);
    }

    return s_sanitized_buffer;
}

void secure_zero_memory(void *ptr, size_t len)
{
    if (ptr == NULL || len == 0) {
        return;
    }

    // Use volatile to prevent compiler optimization
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (len--) {
        *p++ = 0;
    }
}

bool secure_is_sensitive_variable(const char *var_name)
{
    if (var_name == NULL) {
        return false;
    }

    // Convert to lowercase for comparison
    char lower_name[64];
    size_t i;
    for (i = 0; i < sizeof(lower_name) - 1 && var_name[i] != '\0'; i++) {
        lower_name[i] = (char)tolower((unsigned char)var_name[i]);
    }
    lower_name[i] = '\0';

    // Check for common sensitive variable name patterns
    const char *sensitive_patterns[] = {
        "password",
        "passwd",
        "pass",
        "pwd",
        "secret",
        "token",
        "key",
        "credential",
        "auth",
        "api_key",
        "apikey",
        NULL
    };

    for (size_t j = 0; sensitive_patterns[j] != NULL; j++) {
        if (strstr(lower_name, sensitive_patterns[j]) != NULL) {
            return true;
        }
    }

    return false;
}
