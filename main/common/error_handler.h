/**
 * @file error_handler.h
 * @brief Unified error handling framework for KC-Device
 *
 * Provides standardized error handling macros and utilities with:
 * - Context-aware error logging (file, line, function)
 * - Consistent error message formatting
 * - Security-conscious credential handling
 * - Reduced code duplication
 *
 * @note Part of Phase 2 refactoring effort
 */

#ifndef ERROR_HANDLER_H
#define ERROR_HANDLER_H

#include "esp_err.h"
#include "esp_log.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Error severity levels
 * Used to categorize errors by impact
 */
typedef enum {
    ERROR_SEVERITY_DEBUG,      ///< Debug-only, not user-visible
    ERROR_SEVERITY_INFO,       ///< Informational, transient issues
    ERROR_SEVERITY_WARNING,    ///< Warning, degraded functionality
    ERROR_SEVERITY_ERROR,      ///< Error, feature unavailable
    ERROR_SEVERITY_FATAL       ///< Fatal, system cannot continue
} error_severity_t;

/**
 * Error recovery strategies
 */
typedef enum {
    ERROR_RECOVERY_NONE,       ///< No recovery, return error
    ERROR_RECOVERY_RETRY,      ///< Retry operation
    ERROR_RECOVERY_FALLBACK,   ///< Use fallback/degraded mode
    ERROR_RECOVERY_RESET       ///< Reset subsystem
} error_recovery_strategy_t;

/**
 * Log error with context and return error code
 *
 * Internal function - use RETURN_ON_ERROR macro instead
 */
void error_log_and_return(esp_err_t err,
                          const char *message,
                          const char *file,
                          int line,
                          const char *function,
                          const char *tag);

/**
 * Log warning with context
 *
 * Internal function - use WARN_ON_ERROR macro instead
 */
void error_log_warning(esp_err_t err,
                      const char *message,
                      const char *file,
                      int line,
                      const char *function,
                      const char *tag);

/**
 * Log fatal error with context and abort
 *
 * Internal function - use FATAL_ON_ERROR macro instead
 */
void error_log_fatal(esp_err_t err,
                    const char *message,
                    const char *file,
                    int line,
                    const char *function,
                    const char *tag) __attribute__((noreturn));

/**
 * RETURN_ON_ERROR - Check expression result and return on error
 *
 * Evaluates expression, logs error with context if not ESP_OK, and returns error code.
 *
 * @param expr Expression that returns esp_err_t
 * @param msg Error message (string literal or variable)
 *
 * Example:
 *   RETURN_ON_ERROR(wifi_init(), "WiFi initialization failed");
 */
#define RETURN_ON_ERROR(expr, msg) \
    do { \
        esp_err_t _err_code = (expr); \
        if (_err_code != ESP_OK) { \
            error_log_and_return(_err_code, msg, __FILE__, __LINE__, __func__, TAG); \
            return _err_code; \
        } \
    } while(0)

/**
 * RETURN_ON_ERROR_WITH_CLEANUP - Check expression and run cleanup before return
 *
 * @param expr Expression that returns esp_err_t
 * @param msg Error message
 * @param cleanup Cleanup code to run before return
 *
 * Example:
 *   RETURN_ON_ERROR_WITH_CLEANUP(
 *       i2c_master_cmd_begin(...),
 *       "I2C transaction failed",
 *       i2c_cmd_link_delete(cmd)
 *   );
 */
#define RETURN_ON_ERROR_WITH_CLEANUP(expr, msg, cleanup) \
    do { \
        esp_err_t _err_code = (expr); \
        if (_err_code != ESP_OK) { \
            error_log_and_return(_err_code, msg, __FILE__, __LINE__, __func__, TAG); \
            cleanup; \
            return _err_code; \
        } \
    } while(0)

/**
 * WARN_ON_ERROR - Check expression and log warning but continue
 *
 * @param expr Expression that returns esp_err_t
 * @param msg Warning message
 *
 * Example:
 *   WARN_ON_ERROR(optional_feature_init(), "Optional feature unavailable");
 */
#define WARN_ON_ERROR(expr, msg) \
    do { \
        esp_err_t _err_code = (expr); \
        if (_err_code != ESP_OK) { \
            error_log_warning(_err_code, msg, __FILE__, __LINE__, __func__, TAG); \
        } \
    } while(0)

/**
 * FATAL_ON_ERROR - Check expression and abort on error
 *
 * Use for critical system initialization that cannot fail.
 * Provides better error context than ESP_ERROR_CHECK.
 *
 * @param expr Expression that returns esp_err_t
 * @param msg Fatal error message
 *
 * Example:
 *   FATAL_ON_ERROR(nvs_flash_init(), "NVS initialization failed - cannot continue");
 */
#define FATAL_ON_ERROR(expr, msg) \
    do { \
        esp_err_t _err_code = (expr); \
        if (_err_code != ESP_OK) { \
            error_log_fatal(_err_code, msg, __FILE__, __LINE__, __func__, TAG); \
        } \
    } while(0)

/**
 * RETURN_ON_NULL - Check pointer and return ESP_ERR_INVALID_ARG if NULL
 *
 * @param ptr Pointer to check
 * @param msg Error message
 *
 * Example:
 *   RETURN_ON_NULL(config, "Configuration pointer is NULL");
 */
#define RETURN_ON_NULL(ptr, msg) \
    do { \
        if ((ptr) == NULL) { \
            error_log_and_return(ESP_ERR_INVALID_ARG, msg, __FILE__, __LINE__, __func__, TAG); \
            return ESP_ERR_INVALID_ARG; \
        } \
    } while(0)

/**
 * RETURN_ON_FALSE - Check condition and return ESP_ERR_INVALID_STATE if false
 *
 * @param condition Condition to check
 * @param msg Error message
 * @param err_code Error code to return
 *
 * Example:
 *   RETURN_ON_FALSE(sensor_initialized, "Sensor not initialized", ESP_ERR_INVALID_STATE);
 */
#define RETURN_ON_FALSE(condition, msg, err_code) \
    do { \
        if (!(condition)) { \
            error_log_and_return(err_code, msg, __FILE__, __LINE__, __func__, TAG); \
            return err_code; \
        } \
    } while(0)

/**
 * GOTO_ON_ERROR - Check expression and goto label on error
 *
 * Useful for complex cleanup paths
 *
 * @param expr Expression that returns esp_err_t
 * @param label Label to goto on error
 * @param msg Error message
 *
 * Example:
 *   esp_err_t ret = ESP_OK;
 *   GOTO_ON_ERROR(init_step1(), cleanup, "Step 1 failed");
 *   GOTO_ON_ERROR(init_step2(), cleanup, "Step 2 failed");
 *   return ESP_OK;
 *
 *   cleanup:
 *       cleanup_resources();
 *       return ret;
 */
#define GOTO_ON_ERROR(expr, label, msg) \
    do { \
        esp_err_t _err_code = (expr); \
        if (_err_code != ESP_OK) { \
            error_log_and_return(_err_code, msg, __FILE__, __LINE__, __func__, TAG); \
            ret = _err_code; \
            goto label; \
        } \
    } while(0)

#ifdef __cplusplus
}
#endif

#endif // ERROR_HANDLER_H
