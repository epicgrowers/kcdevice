# Phase 2: Error Handling & Safety - Detailed Documentation

**Status**: In Progress
**Priority**: High
**Effort**: Medium
**Impact**: Very High

## Objectives

1. Create unified error handling framework
2. Standardize error reporting patterns across modules
3. Improve credential security handling
4. Reduce code duplication in error paths
5. Add context-aware error logging

---

## Current State Analysis

### Error Handling Patterns Found

Based on initial analysis:
- **334+ ESP_ERR checks** across codebase
- **796 log statements** (mix of ERROR, WARN, INFO, DEBUG)
- **Multiple patterns**:
  - Direct `ESP_ERROR_CHECK()` (crashes on error)
  - Manual `if (ret != ESP_OK)` checks with logging
  - Silent failures (check but don't log)
  - Inconsistent error message formats

### Problems Identified

1. **Inconsistent Error Reporting**
   ```c
   // Pattern 1: Verbose
   if (ret != ESP_OK) {
       ESP_LOGE(TAG, "Failed to init sensor: %s", esp_err_to_name(ret));
       return ret;
   }

   // Pattern 2: Terse
   if (ret != ESP_OK) return ret;

   // Pattern 3: Crash on error
   ESP_ERROR_CHECK(esp_wifi_init(&cfg));
   ```

2. **Missing Context**
   - Error messages lack file/line information
   - No indication of impact (fatal vs recoverable)
   - Missing user-facing error guidance

3. **Code Duplication**
   - Same error handling logic repeated
   - Inconsistent retry strategies
   - No centralized error recovery

4. **Security Concerns**
   - Credentials potentially logged in error messages
   - Sensitive data in memory after errors
   - No secure cleanup on failure paths

---

## Solution Design

### 1. Error Handler Framework

Create a lightweight error handling framework with:

```c
// main/common/error_handler.h

/**
 * Error context - provides rich error information
 */
typedef struct {
    esp_err_t code;              // ESP error code
    const char *message;         // Human-readable message
    const char *file;            // Source file
    int line;                    // Line number
    const char *function;        // Function name
    bool is_fatal;               // Fatal error flag
} error_context_t;

/**
 * Error handling macros with context
 */

// Return on error with context logging
#define RETURN_ON_ERROR(expr, msg) \
    do { \
        esp_err_t _err = (expr); \
        if (_err != ESP_OK) { \
            error_log_and_return(_err, msg, __FILE__, __LINE__, __func__); \
            return _err; \
        } \
    } while(0)

// Check error but continue
#define WARN_ON_ERROR(expr, msg) \
    do { \
        esp_err_t _err = (expr); \
        if (_err != ESP_OK) { \
            error_log_warning(_err, msg, __FILE__, __LINE__, __func__); \
        } \
    } while(0)

// Fatal error - log and abort
#define FATAL_ON_ERROR(expr, msg) \
    do { \
        esp_err_t _err = (expr); \
        if (_err != ESP_OK) { \
            error_log_fatal(_err, msg, __FILE__, __LINE__, __func__); \
            abort(); \
        } \
    } while(0)

// Null pointer check
#define RETURN_ON_NULL(ptr, msg) \
    do { \
        if ((ptr) == NULL) { \
            error_log_and_return(ESP_ERR_INVALID_ARG, msg, __FILE__, __LINE__, __func__); \
            return ESP_ERR_INVALID_ARG; \
        } \
    } while(0)
```

### 2. Error Categories

Define error severity levels:

```c
typedef enum {
    ERROR_SEVERITY_DEBUG,      // Debug-only, not user-visible
    ERROR_SEVERITY_INFO,       // Informational, transient issues
    ERROR_SEVERITY_WARNING,    // Warning, degraded functionality
    ERROR_SEVERITY_ERROR,      // Error, feature unavailable
    ERROR_SEVERITY_FATAL       // Fatal, system cannot continue
} error_severity_t;
```

### 3. Secure Error Logging

```c
/**
 * Secure logging - never logs sensitive data
 */
void error_log_secure(esp_err_t code,
                      const char *message,
                      bool has_sensitive_data);

/**
 * Sanitize strings before logging
 */
const char* error_sanitize_string(const char *str,
                                  bool is_password);
```

### 4. Error Recovery Strategies

```c
typedef enum {
    ERROR_RECOVERY_NONE,       // No recovery, return error
    ERROR_RECOVERY_RETRY,      // Retry operation
    ERROR_RECOVERY_FALLBACK,   // Use fallback/degraded mode
    ERROR_RECOVERY_RESET       // Reset subsystem
} error_recovery_strategy_t;

/**
 * Execute operation with retry strategy
 */
esp_err_t error_execute_with_retry(
    esp_err_t (*operation)(void *ctx),
    void *context,
    uint32_t max_retries,
    uint32_t retry_delay_ms
);
```

---

## Implementation Plan

### Phase 2.1: Framework Creation (Current)

**Goal**: Create reusable error handling utilities

**Tasks**:
1. Create `main/common/error_handler.h` - Error handling macros
2. Create `main/common/error_handler.c` - Implementation
3. Create `main/common/secure_logging.h` - Secure logging utilities
4. Create `main/common/secure_logging.c` - Implementation
5. Add unit tests for error handlers

**Files to Create**:
- `main/common/error_handler.h`
- `main/common/error_handler.c`
- `main/common/secure_logging.h`
- `main/common/secure_logging.c`

### Phase 2.2: Credential Security

**Goal**: Ensure credentials never leak in logs

**Tasks**:
1. Audit all credential handling code
2. Add secure cleanup on error paths
3. Implement credential sanitization
4. Add compile-time checks for credential logging

**Files to Audit**:
- `main/provisioning/wifi_manager.c`
- `main/provisioning/provisioning_runner.c`
- `main/services/keys/api_key_manager.c`
- `main/services/provisioning/cloud_provisioning.c`

### Phase 2.3: Refactor Error Handling

**Goal**: Apply new framework to existing code

**Strategy**: Start with high-impact modules first
1. WiFi manager (high error rate, user-facing)
2. Sensor manager (complex retry logic)
3. Services core (orchestration errors)
4. MQTT telemetry (network errors)

**Approach**:
- Replace verbose error handling with macros
- Add context to error messages
- Implement proper cleanup on failures
- Add recovery strategies where appropriate

### Phase 2.4: Documentation & Testing

**Goal**: Ensure framework is well-documented and tested

**Tasks**:
1. Update refactoring log
2. Create error handling best practices guide
3. Add examples to documentation
4. Test all error paths
5. Verify no sensitive data leaks

---

## Expected Outcomes

### Code Quality Improvements

- **Consistency**: All errors handled uniformly
- **Maintainability**: Centralized error handling logic
- **Debugging**: Rich context in error messages
- **Security**: No credential leaks

### Metrics

**Before**:
- 334+ manual error checks
- Inconsistent error messages
- No standardized recovery
- Potential security issues

**After** (Target):
- ~200 uses of error macros (consolidation)
- Consistent, context-rich error messages
- Standardized retry/recovery strategies
- Verified secure credential handling

### Developer Experience

- **Faster debugging**: File/line/function in logs
- **Clear patterns**: Obvious how to handle errors
- **Reduced boilerplate**: Macros eliminate repetition
- **Better security**: Can't accidentally log credentials

---

## Risk Assessment

### Low Risk
- ✅ Macro-based approach is backwards compatible
- ✅ Can be adopted incrementally
- ✅ No breaking API changes

### Medium Risk
- ⚠️ Need to verify all error paths still work
- ⚠️ Macro evaluation order matters
- ⚠️ Must test both success and failure cases

### Mitigation
- Thorough testing of refactored modules
- Keep original error handling during transition
- Incremental rollout, one module at a time

---

## Success Criteria

- [ ] Error handler framework compiles without warnings
- [ ] All refactored modules build successfully
- [ ] No credentials appear in logs (audit verified)
- [ ] Error messages include file/line/function context
- [ ] Retry logic uses centralized implementation
- [ ] Documentation updated with examples
- [ ] Code review completed
- [ ] ESP32-S3 and ESP32-C6 builds pass

---

## Next Actions

1. Create error handler framework files
2. Implement core macros and utilities
3. Refactor WiFi manager as proof of concept
4. Gather feedback and iterate
5. Roll out to remaining modules
