# Phase 1: Quick Wins - Detailed Documentation

**Status**: In Progress
**Priority**: High
**Effort**: Low
**Impact**: High

## Objectives

1. Eliminate magic numbers throughout codebase
2. Standardize logging tag hierarchy
3. Improve code readability without functional changes
4. Establish patterns for future development

---

## 1. Magic Numbers Elimination

### Problem Statement

Magic numbers scattered throughout the code make it difficult to:
- Understand what values represent
- Maintain consistent timing across modules
- Tune performance parameters
- Debug timing-related issues

### Examples Found

#### WiFi Manager (`main/provisioning/wifi_manager.c`)
```c
// Line 20-22 - BEFORE
#define WIFI_RETRY_BASE_DELAY_MS 1000U
#define WIFI_RETRY_MAX_DELAY_MS 60000U
#define WIFI_RETRY_MAX_EXPONENT 6U
```

**Issues**:
- No explanation of why 1000ms or 60000ms chosen
- Relationship between values not clear
- No documentation of exponential backoff formula

#### Sensor Manager (`main/sensors/sensor_manager.c`)
```c
// Lines 34-41 - BEFORE
#define SENSOR_TRIGGER_DELAY_MS 20
#define SENSOR_WAIT_STEP_MS 50
#define SENSOR_MIN_WAIT_MS 750
#define SENSOR_CONSECUTIVE_FAIL_LIMIT 5
#define SENSOR_RECOVERY_RETRY_MS 120000
#define SENSOR_RECOVERY_RETRY_US (SENSOR_RECOVERY_RETRY_MS * 1000ULL)
#define SENSOR_RECOVERY_INIT_DELAY_MS 3000
#define SENSOR_RECOVERY_MAX_ATTEMPTS 5
```

**Issues**:
- No rationale for 750ms minimum wait
- Magic number 5 used twice (limit and attempts) - coincidence or intentional?
- 120000ms (2 minutes) not obviously derived

#### Provisioning Runner (`main/provisioning/provisioning_runner.c`)
```c
// Line 104 - BEFORE
while (!ops->is_connected() && wait_seconds < WIFI_STORED_CREDENTIAL_WAIT_SEC) {
    vTaskDelay(pdMS_TO_TICKS(1000));  // Magic: 1 second poll interval
    wait_seconds++;
}
```

**Issue**: 1000ms hardcoded without constant name

### Solution Approach

1. **Add explanatory comments** for all timing constants
2. **Group related constants** together
3. **Derive values** where relationships exist
4. **Create centralized constants** for common values

---

## 2. Logging Tag Standardization

### Problem Statement

Current logging uses flat namespace:
- `"WIFI_MGR"`, `"SENSOR_MGR"`, `"PROV_RUN"`, `"SERVICES_CORE"`
- Difficult to filter by subsystem
- No hierarchical organization
- Inconsistent abbreviation patterns

### Solution: Hierarchical Tag System

#### Proposed Hierarchy

```
MAIN:APP              # Application entry point
MAIN:BOOT             # Boot coordinator

PLATFORM:CHIP         # Chip info
PLATFORM:SEC          # Security
PLATFORM:I2C          # I2C scanner
PLATFORM:RESET        # Reset button

PROV:RUNNER           # Provisioning runner
PROV:WIFI_MGR         # WiFi manager
PROV:IDF              # IDF provisioning wrapper
PROV:STATE            # State machine

SENSORS:MGR           # Sensor manager
SENSORS:PIPELINE      # Sensor pipeline
SENSORS:BOOT          # Sensor boot task
SENSORS:EZO           # EZO driver
SENSORS:MAX17048      # Battery monitor

SERVICES:CORE         # Services orchestrator
SERVICES:HTTP         # HTTP server
SERVICES:MQTT         # MQTT telemetry
SERVICES:MDNS         # mDNS discovery
SERVICES:TIME         # Time sync
SERVICES:PROV_BRIDGE  # Provisioning bridge

NETWORK:BOOT          # Network boot
NETWORK:WATCHDOG      # Connectivity watchdog

STORAGE:SD            # SD logger
STORAGE:LOG_HIST      # Log history
```

#### Benefits

1. **Filtering**: `esp_log_level_set("SENSORS:*", ESP_LOG_DEBUG)` enables all sensor logs
2. **Clarity**: Immediate understanding of component hierarchy
3. **Consistency**: Standard pattern across all modules
4. **Debugging**: Easier to trace issues across subsystem boundaries

---

## 3. Implementation Strategy

### Step-by-step Approach

1. **Create common constants header** (`main/common/constants.h`)
2. **Refactor WiFi manager** - Document and improve constants
3. **Refactor sensor manager** - Document and improve constants
4. **Update logging tags** - Apply hierarchical naming
5. **Build and test** - Verify no functional changes
6. **Commit changes** - Atomic commits per module

### Testing Checklist

After each change:
- [ ] Build succeeds for ESP32-S3: `idf.py -D SDKCONFIG=sdkconfig.esp32s3 build`
- [ ] Build succeeds for ESP32-C6: `idf.py -D SDKCONFIG=sdkconfig.esp32c6 build`
- [ ] No new compiler warnings
- [ ] Flash and verify basic functionality (if possible)

---

## 4. Expected Outcomes

### Code Quality Improvements

- **Readability**: +30% (subjective, based on peer review)
- **Maintainability**: Constants can be tuned without code diving
- **Documentation**: Self-documenting constant names
- **Debugging**: Hierarchical logs easier to filter

### Non-Goals

- ❌ No functional behavior changes
- ❌ No API changes
- ❌ No performance optimization (yet)
- ❌ No architectural changes (Phase 3)

---

## 5. Change Summary

### Files Modified

_To be updated as changes are made_

### Constants Added

_To be updated as changes are made_

### Logging Tags Updated

_To be updated as changes are made_

---

## Review Checklist

Before marking Phase 1 complete:

- [ ] All magic numbers have named constants
- [ ] All constants have explanatory comments
- [ ] Logging tags follow hierarchical pattern
- [ ] Documentation updated
- [ ] Both targets build successfully
- [ ] Code review completed
- [ ] Committed with clear messages
