/**
 * @file secure_logging.h
 * @brief Secure logging utilities to prevent credential leaks
 *
 * Provides utilities to safely log strings that may contain sensitive data:
 * - Passwords
 * - API keys
 * - Tokens
 * - SSIDs (optionally masked)
 *
 * @note Part of Phase 2 refactoring effort - Security improvements
 */

#ifndef SECURE_LOGGING_H
#define SECURE_LOGGING_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Maximum length for sanitized string output
 */
#define SECURE_LOG_MAX_LEN 64

/**
 * Sanitize a string for safe logging
 *
 * Returns a masked version of the string to prevent sensitive data leaks.
 * The returned string is stored in a static buffer and will be overwritten
 * on the next call.
 *
 * @param str String to sanitize (can be NULL)
 * @param is_password If true, completely masks the string; if false, shows length
 * @return Sanitized string safe for logging
 *
 * Examples:
 *   secure_sanitize_string("MyPassword123", true) -> "******* (13 chars)"
 *   secure_sanitize_string("MySSID", false) -> "My*** (6 chars)"
 *   secure_sanitize_string(NULL, true) -> "<null>"
 */
const char* secure_sanitize_string(const char *str, bool is_password);

/**
 * Mask password for logging
 *
 * Completely masks a password string.
 *
 * @param password Password to mask (can be NULL)
 * @return Masked string showing only length
 *
 * Example:
 *   secure_mask_password("SuperSecret!") -> "******* (12 chars)"
 */
const char* secure_mask_password(const char *password);

/**
 * Partially mask SSID for logging
 *
 * Shows first 2 characters and masks the rest.
 * Useful for debugging while maintaining some privacy.
 *
 * @param ssid SSID to partially mask (can be NULL)
 * @return Partially masked SSID
 *
 * Example:
 *   secure_mask_ssid("MyHomeWiFi") -> "My******* (10 chars)"
 */
const char* secure_mask_ssid(const char *ssid);

/**
 * Mask API key for logging
 *
 * Shows only first 4 and last 4 characters of API key.
 *
 * @param api_key API key to mask (can be NULL)
 * @return Masked API key
 *
 * Example:
 *   secure_mask_api_key("1234567890abcdef") -> "1234...cdef"
 */
const char* secure_mask_api_key(const char *api_key);

/**
 * Secure zero memory
 *
 * Zeros memory in a way that won't be optimized away by compiler.
 * Use this to clear sensitive data like passwords after use.
 *
 * @param ptr Pointer to memory to zero
 * @param len Length of memory to zero
 *
 * Example:
 *   char password[64];
 *   // ... use password ...
 *   secure_zero_memory(password, sizeof(password));
 */
void secure_zero_memory(void *ptr, size_t len);

/**
 * Check if string appears to be sensitive
 *
 * Heuristic check to detect if a string might contain sensitive data.
 * Checks for common patterns in variable names.
 *
 * @param var_name Variable name to check
 * @return true if variable name suggests sensitive data
 *
 * Note: This is a heuristic helper. Always explicitly mark known-sensitive data.
 */
bool secure_is_sensitive_variable(const char *var_name);

#ifdef __cplusplus
}
#endif

#endif // SECURE_LOGGING_H
