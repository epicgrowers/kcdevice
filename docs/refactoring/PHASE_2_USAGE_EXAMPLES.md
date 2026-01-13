# Phase 2: Error Handling Framework - Usage Examples

This document provides practical examples of using the new error handling framework.

## Quick Start

### Include the Headers

```c
#include "common/error_handler.h"
#include "common/secure_logging.h"
```

---

## Error Handling Examples

### Example 1: Basic Error Checking

**Before** (Old Pattern):
```c
esp_err_t ret = sensor_init();
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize sensor: %s", esp_err_to_name(ret));
    return ret;
}
```

**After** (New Pattern):
```c
RETURN_ON_ERROR(sensor_init(), "Failed to initialize sensor");
```

**Output Log**:
```
E (1234) SENSORS:MGR: Failed to initialize sensor [sensor_manager.c:142 in sensor_manager_init()] - Error: ESP_ERR_TIMEOUT (0x107)
```

---

### Example 2: Error with Cleanup

**Before**:
```c
i2c_cmd_handle_t cmd = i2c_cmd_link_create();
esp_err_t ret = i2c_master_start(cmd);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "I2C start failed: %s", esp_err_to_name(ret));
    i2c_cmd_link_delete(cmd);
    return ret;
}
```

**After**:
```c
i2c_cmd_handle_t cmd = i2c_cmd_link_create();
RETURN_ON_ERROR_WITH_CLEANUP(
    i2c_master_start(cmd),
    "I2C start failed",
    i2c_cmd_link_delete(cmd)
);
```

---

### Example 3: Warning on Non-Critical Errors

**Use Case**: Optional feature that can fail without affecting core functionality

```c
// Try to enable advanced feature, but don't fail if unavailable
WARN_ON_ERROR(
    enable_advanced_mode(),
    "Advanced mode unavailable - continuing with basic features"
);
```

**Output** (if fails):
```
W (5678) SERVICES:CORE: Advanced mode unavailable - continuing with basic features [services.c:89 in services_start()] - Warning: ESP_ERR_NOT_SUPPORTED (0x106)
```

---

### Example 4: Fatal Errors

**Use Case**: Critical system initialization that must succeed

**Before**:
```c
ESP_ERROR_CHECK(nvs_flash_init());
```

**After** (provides better context):
```c
FATAL_ON_ERROR(nvs_flash_init(), "NVS initialization failed - cannot continue");
```

**Output** (on error):
```
E (123) PLATFORM:SEC: FATAL: NVS initialization failed - cannot continue [security.c:45 in security_init()] - Error: ESP_ERR_NVS_CORRUPT (0x1101)
E (124) PLATFORM:SEC: System cannot continue - aborting
abort() was called at PC 0x40084abc on core 0
```

---

### Example 5: Null Pointer Checks

**Before**:
```c
if (config == NULL) {
    ESP_LOGE(TAG, "Configuration pointer is NULL");
    return ESP_ERR_INVALID_ARG;
}
```

**After**:
```c
RETURN_ON_NULL(config, "Configuration pointer is NULL");
```

---

### Example 6: Condition Checks

**Before**:
```c
if (!sensor_initialized) {
    ESP_LOGE(TAG, "Sensor not initialized");
    return ESP_ERR_INVALID_STATE;
}
```

**After**:
```c
RETURN_ON_FALSE(sensor_initialized, "Sensor not initialized", ESP_ERR_INVALID_STATE);
```

---

### Example 7: Complex Cleanup with GOTO

**Use Case**: Multiple initialization steps with cleanup needed

```c
esp_err_t complex_init(void)
{
    esp_err_t ret = ESP_OK;
    void *resource1 = NULL;
    void *resource2 = NULL;

    resource1 = allocate_resource1();
    RETURN_ON_NULL(resource1, "Failed to allocate resource1");

    GOTO_ON_ERROR(
        initialize_step1(resource1),
        cleanup,
        "Step 1 initialization failed"
    );

    resource2 = allocate_resource2();
    if (resource2 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate resource2");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    GOTO_ON_ERROR(
        initialize_step2(resource2),
        cleanup,
        "Step 2 initialization failed"
    );

    // Success path
    return ESP_OK;

cleanup:
    if (resource2) free_resource2(resource2);
    if (resource1) free_resource1(resource1);
    return ret;
}
```

---

## Secure Logging Examples

### Example 1: Mask Passwords

**Before** (INSECURE):
```c
ESP_LOGI(TAG, "Connecting to WiFi SSID: %s with password: %s", ssid, password);
```

**After** (SECURE):
```c
ESP_LOGI(TAG, "Connecting to WiFi SSID: %s with password: %s",
         secure_mask_ssid(ssid),
         secure_mask_password(password));
```

**Output**:
```
I (1234) PROV:WIFI_MGR: Connecting to WiFi SSID: My*** (10 chars) with password: ******* (12 chars)
```

---

### Example 2: Mask API Keys

```c
ESP_LOGI(TAG, "Using API key: %s", secure_mask_api_key(api_key));
```

**Output**:
```
I (5678) SERVICES:KEYS: Using API key: sk_t...4x9z
```

---

### Example 3: Secure Memory Cleanup

**Before**:
```c
char password[64];
// ... use password ...
memset(password, 0, sizeof(password));  // May be optimized away!
```

**After** (compiler-safe):
```c
char password[64];
// ... use password ...
secure_zero_memory(password, sizeof(password));  // Never optimized away
```

---

### Example 4: Check for Sensitive Variables

```c
void log_variable(const char *var_name, const char *value)
{
    if (secure_is_sensitive_variable(var_name)) {
        ESP_LOGI(TAG, "%s = %s", var_name, secure_sanitize_string(value, true));
    } else {
        ESP_LOGI(TAG, "%s = %s", var_name, value);
    }
}

// Usage:
log_variable("api_key", "secret123");     // Logs: api_key = ******* (9 chars)
log_variable("device_id", "ESP32-1234");  // Logs: device_id = ESP32-1234
```

---

## Real-World Refactoring Example

### WiFi Manager Connection Function

**Before**:
```c
esp_err_t wifi_manager_connect(const char* ssid, const char* password)
{
    if (ssid == NULL || password == NULL) {
        ESP_LOGE(TAG, "SSID or password is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid);

    // Store credentials temporarily
    strncpy(pending_ssid, ssid, sizeof(pending_ssid) - 1);
    strncpy(pending_password, password, sizeof(pending_password) - 1);

    // Configure WiFi
    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    // Stop WiFi if already running
    esp_wifi_stop();

    // Set configuration
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(ret));
        return ret;
    }

    // ... more code ...
    return ESP_OK;
}
```

**After** (with new framework):
```c
esp_err_t wifi_manager_connect(const char* ssid, const char* password)
{
    RETURN_ON_NULL(ssid, "SSID is NULL");
    RETURN_ON_NULL(password, "Password is NULL");

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", secure_mask_ssid(ssid));

    // Store credentials temporarily
    strncpy(pending_ssid, ssid, sizeof(pending_ssid) - 1);
    strncpy(pending_password, password, sizeof(pending_password) - 1);

    // Configure WiFi
    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    // Stop WiFi if already running
    esp_wifi_stop();

    // Set configuration
    RETURN_ON_ERROR(
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config),
        "Failed to set WiFi configuration"
    );

    // Secure cleanup
    secure_zero_memory(&wifi_config, sizeof(wifi_config));

    // ... more code ...
    return ESP_OK;
}
```

**Benefits**:
- 7 lines → 2 lines for NULL checks
- 5 lines → 1 line for error handling
- Added SSID masking for privacy
- Added secure memory cleanup
- Better error context in logs

---

## Best Practices

### 1. Always Use Secure Logging for Credentials

❌ **Bad**:
```c
ESP_LOGI(TAG, "Password: %s", password);
```

✅ **Good**:
```c
ESP_LOGI(TAG, "Password: %s", secure_mask_password(password));
```

### 2. Clear Sensitive Data After Use

❌ **Bad**:
```c
char password[64];
// ... use password ...
// password left in memory!
```

✅ **Good**:
```c
char password[64];
// ... use password ...
secure_zero_memory(password, sizeof(password));
```

### 3. Use Appropriate Error Macro

- **RETURN_ON_ERROR**: Standard error handling
- **WARN_ON_ERROR**: Non-critical, optional features
- **FATAL_ON_ERROR**: Critical initialization only
- **GOTO_ON_ERROR**: Complex cleanup scenarios

### 4. Provide Clear Error Messages

❌ **Bad**:
```c
RETURN_ON_ERROR(init(), "Failed");
```

✅ **Good**:
```c
RETURN_ON_ERROR(sensor_init(), "Sensor initialization failed - check I2C connection");
```

### 5. Don't Nest Error Macros

❌ **Bad**:
```c
RETURN_ON_ERROR(
    RETURN_ON_NULL(config, "Config null"),  // Don't do this!
    "Error"
);
```

✅ **Good**:
```c
RETURN_ON_NULL(config, "Configuration pointer is NULL");
RETURN_ON_ERROR(process_config(config), "Failed to process configuration");
```

---

## Migration Checklist

When refactoring a module to use the new framework:

- [ ] Add `#include "common/error_handler.h"` at top
- [ ] Replace manual error checks with appropriate macros
- [ ] Identify sensitive data (passwords, keys, tokens)
- [ ] Add `#include "common/secure_logging.h"` if needed
- [ ] Replace credential logging with secure variants
- [ ] Add `secure_zero_memory()` for credential cleanup
- [ ] Build and test
- [ ] Verify error messages include context
- [ ] Verify no credentials in logs
- [ ] Update documentation

---

## Performance Notes

- **Minimal overhead**: Macros expand inline, no function call overhead
- **No runtime cost**: Error path only executed on actual errors
- **Compile-time safe**: Type checking preserved
- **Debug-friendly**: Full context in logs aids debugging

---

## Questions?

See also:
- [PHASE_2_ERROR_HANDLING.md](PHASE_2_ERROR_HANDLING.md) - Overall design
- [REFACTORING_LOG.md](REFACTORING_LOG.md) - Complete change log
- `main/common/error_handler.h` - Full API documentation
- `main/common/secure_logging.h` - Security utilities
