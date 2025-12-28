# Project Structure Guide

This document explains the organization of the KC-Device WiFi BLE Provisioning project, supporting both ESP32-S3 and ESP32-C6 hardware variants.

## Directory Layout

```
kc_device/
├── README.md                   # Quick start guide and project overview
├── CMakeLists.txt              # Root CMake configuration
├── build.ps1                   # Build script for target switching
├── sdkconfig.s3                # ESP32-S3 SDK configuration
├── sdkconfig.c6                # ESP32-C6 SDK configuration
│
├── main/                       # Application source code
│   ├── CMakeLists.txt         # Main component build configuration
│   ├── main.c                 # Application entry point and orchestration
│   ├── provisioning/          # Provisioning subsystem
│   │   ├── provisioning_runner.c/.h  # Stored-credential + BLE orchestration
│   │   ├── idf_provisioning.c/.h     # ESP-IDF provisioning manager wrapper
│   │   ├── provisioning_state.c/.h   # Shared provisioning state machine
│   │   └── wifi_manager.c/.h         # WiFi connection and NVS storage
│   ├── sensors/               # Sensor subsystem
│   │   ├── sensor_boot.c/.h         # Sensor-task launcher + boot glue
│   │   ├── pipeline.c/.h           # Boot-facing wrapper that launches the sensor task
│   │   ├── sensor_manager.c/.h      # Aggregates drivers, caches readings
│   │   └── drivers/
│   │       ├── ezo_sensor.c/.h      # Atlas Scientific EZO driver helpers
│   │       └── max17048.c/.h        # MAX17048 battery monitor driver
│   ├── services/             # Network + cloud services subsystem
│   │   ├── core/             # services_start()/services_stop() + shared config structs
│   │   ├── http/             # Dashboard HTTP(S) server, REST handlers, WebSocket streaming
│   │   ├── telemetry/        # MQTT telemetry clients + future analytics tasks
│   │   ├── discovery/        # mDNS and LAN service advertisement
│   │   ├── time_sync/        # SNTP time sync helpers
│   │   └── keys/             # API key storage and helpers
│
├── config/                     # Configuration files
│   ├── runtime/               # Embedded JSON/TOML overrides consumed at boot
│   ├── sdkconfig.defaults     # Default ESP32-S3 configuration
│   ├── sdkconfig.esp32c6      # Default ESP32-C6 configuration
│   └── partitions.csv         # Flash partition table
│
├── docs/                       # Documentation
│   ├── README.md              # Complete project documentation
│   ├── KOTLIN_INTEGRATION.md  # Android/Kotlin integration guide
│   └── PROJECT_STRUCTURE.md   # This file
│
├── test/                       # Test files and examples
│   ├── test_credentials.json  # Sample credentials for testing
│   ├── test_credentials_examples.txt
│   └── test_provisioning_state_stub.py  # Host-side provisioning state test scaffolding
│
├── build_s3/                   # ESP32-S3 build artifacts (gitignored)
│   ├── kc_device.bin
│   ├── kc_device.elf
│   ├── kc_device.map
│   ├── bootloader/
│   └── partition_table/
│
├── build_c6/                   # ESP32-C6 build artifacts (gitignored)
│   ├── kc_device.bin
│   ├── kc_device.elf
│   ├── kc_device.map
│   ├── bootloader/
│   └── partition_table/
│
└── .git/                       # Git version control

```

## Source Code Organization

### Main Component (`main/`)

#### `main.c` (170 lines)
**Purpose**: Application entry point and system orchestration

**Responsibilities**:
- Initialize NVS flash storage
- Initialize state machine
- Check for stored WiFi credentials
- Start BLE provisioning if needed
- Handle auto-reconnect with stored credentials
- Coordinate between BLE and WiFi components

**Key Functions**:
- `app_main()` - Entry point
- `state_change_handler()` - Callback for state machine events

---

#### `boot/boot_coordinator.c/h`
**Purpose**: Central boot ownership for shared event groups, sensor/task launches, and readiness waits

**Responsibilities**:
- Create and own the boot event group used by sensors, network services, and Wi-Fi boot code
- Provide canonical `sensor_pipeline_launch_ctx_t` / `network_boot_config_t` instances for the respective boot tasks
- Launch the sensor task idempotently (either during provisioning or afterwards) and expose wait helpers for readiness bits

**Key Functions**:
- `boot_coordinator_init()` - Allocates the event group and seeds config structs
- `boot_coordinator_launch_sensor_task()` / `_launch_sensors_async()` - Ensures the sensor task starts exactly once
- `boot_coordinator_wait_bits()` - Shared wait helper for sensor/network readiness bits

---

#### `provisioning/provisioning_runner.c/h`
**Purpose**: Single entry point that owns Wi-Fi bring-up regardless of whether credentials already exist.

**Responsibilities**:
- Attempt stored-credential reconnection before falling back to BLE provisioning.
- Kick off long-running work (sensor boot) while provisioning waits for user input.
- Invoke `idf_provisioning_start()` / `_stop()` and translate the result into a concise `provisioning_outcome_t`.
- Cache credentials and expose `provisioning_connection_guard_poll()` so reconnection logic stays out of `main.c`.

**Key Functions**:
- `provisioning_run()` - Accepts a `provisioning_plan_t` describing callbacks, event bits, and retry policy.
- `provisioning_connection_guard_poll()` - Periodically verifies link health and re-issues `wifi_manager_connect()` when needed.
- `provisioning_get_saved_network()` / `provisioning_clear_saved_network()` / `provisioning_disconnect()` - Public helpers so the rest of the firmware can inspect or clear Wi-Fi state without talking to `wifi_manager` directly.
- `log_sensor_progress()` (internal) - Mirrors provisioning wait time against sensor readiness for better logs.

---

#### `provisioning/idf_provisioning.c/h`
**Purpose**: Thin wrapper around ESP-IDF's Wi-Fi provisioning manager

**Responsibilities**:
- Configure the provisioning manager to use BLE transport + Security 1 (PoP)
- Generate deterministic BLE service names (`kc-<MAC>`)
- Register ESP-IDF provisioning, Wi-Fi, and IP event handlers
- Translate provisioning events into local state-machine updates
- Invoke `wifi_manager_connect()` when credentials arrive and stop provisioning once Wi-Fi succeeds

**Key Functions**:
- `idf_provisioning_start()` - Initialize the provisioning manager and start advertising
- `idf_provisioning_stop()` - Stop provisioning and free BLE/BTDM resources
- `idf_provisioning_is_running()` - Helper for guarding duplicate starts/stops
- `idf_provisioning_get_service_name()` / `_get_pop()` - Reusable metadata for logging/UI

**Security**:
- ESP-IDF Security 1 (X25519 key exchange + Proof-of-Possession)
- PoP stored in firmware constant (`kPop`)
- BLE controller configured for BLE-only mode; BTDM freed after provisioning ends

---

#### `provisioning/wifi_manager.c/h` (338 lines)
**Purpose**: WiFi connection lifecycle management

**Responsibilities**:
- Initialize WiFi subsystem
- Connect to WiFi networks
- Handle connection events (connect, disconnect, IP assignment)
- Retry logic (5 attempts with exponential backoff)
- Save credentials to NVS
- Retrieve stored credentials
- Error mapping and reporting

**Key Functions**:
- `wifi_manager_init()` - Initialize WiFi
- `wifi_manager_connect()` - Connect to network
- `wifi_manager_get_stored_credentials()` - Retrieve from NVS
- `wifi_manager_clear_credentials()` - Factory reset
- `wifi_event_handler()` - Event-driven WiFi handling
- `save_credentials_to_nvs()` - Persist credentials

**NVS Storage**:
- Namespace: `wifi_config`
- Keys: `ssid`, `password`, `provisioned`

---

#### `provisioning/provisioning_state.c/h` (127 lines)
**Purpose**: Provisioning state machine

**States** (8):
- `PROV_STATE_IDLE` - Waiting for BLE connection
- `PROV_STATE_BLE_CONNECTED` - Client connected
- `PROV_STATE_CREDENTIALS_RECEIVED` - JSON parsed successfully
- `PROV_STATE_WIFI_CONNECTING` - Attempting WiFi connection
- `PROV_STATE_WIFI_CONNECTED` - Connected to WiFi
- `PROV_STATE_PROVISIONED` - Credentials saved, provisioning complete
- `PROV_STATE_WIFI_FAILED` - Connection failed after retries
- `PROV_STATE_ERROR` - Error during provisioning

**Status Codes** (8):
- `STATUS_SUCCESS`
- `STATUS_ERROR_INVALID_JSON`
- `STATUS_ERROR_MISSING_SSID`
- `STATUS_ERROR_MISSING_PASSWORD`
- `STATUS_ERROR_WIFI_TIMEOUT`
- `STATUS_ERROR_WIFI_AUTH_FAILED`
- `STATUS_ERROR_WIFI_NO_AP_FOUND`
- `STATUS_ERROR_STORAGE_FAILED`

**Key Functions**:
- `provisioning_state_init()` - Initialize state machine
- `provisioning_state_set()` - Transition to new state
- `provisioning_state_get()` - Get current state
- `provisioning_state_register_callback()` - Register state change callback
- `provisioning_state_to_string()` - State to string conversion
- `provisioning_status_to_string()` - Status code to string conversion

---

#### `sensors/sensor_boot.c/h`
**Purpose**: Own the dedicated sensor task, coordinating staged initialization during boot.

**Responsibilities**:
- Delay boot briefly so I2C peripherals stabilize, then run a discovery scan for telemetry logging.
- Retry `sensor_manager_init()` a configurable number of times before degrading gracefully.
- Publish readiness bits and summaries back through the boot coordinator/event group.

**Key Functions**:
- `sensor_boot_start()` / `sensor_boot_configure()` - Register the launch context with the boot coordinator.
- `sensor_task()` (internal) - Drives I2C scanning, sensor-manager init, and final readiness notification.

---

#### `sensors/pipeline.c/h`
**Purpose**: Thin orchestration layer that bridges the boot coordinator and the sensor task.

**Responsibilities**:
- Hold the boot event group, readiness bit, and sampling interval supplied by the coordinator.
- Provide synchronous and asynchronous launch helpers used during provisioning (so sensor work can start while BLE is active).
- Track whether the sensor task has already been requested to avoid duplicate launches.

**Key Functions**:
- `sensor_pipeline_prepare()` - Populate a launch context with event bits + interval metadata.
- `sensor_pipeline_launch()` / `_launch_async()` - Trigger `sensor_boot_start()` and flip the launched flag when successful.

---

#### `sensors/sensor_manager.c/h`
**Purpose**: Aggregate all attached sensors (EZO stack + MAX17048) behind one API and maintain cached readings for HTTP/MQTT consumers.

**Responsibilities**:
- Track all EZO devices discovered on the bus, issue measurement commands, and decode multi-value payloads.
- Maintain background sampling tasks, pause/resume hooks, and a shared `sensor_cache_t` for dashboards/telemetry.
- Provide helper reads for temperature, EC, DO, ORP, humidity, and battery voltage/percentage.
- Surface guard utilities so HTTP handlers can safely pause acquisition while calibrating.

**Key Functions**:
- `sensor_manager_init()` / `_deinit()` - Discover hardware and start background tasks.
- `sensor_manager_get_cached_data()` - Return the latest thread-safe snapshot.
- `sensor_manager_pause_reading()` / `_resume_reading()` - Allow dashboard operations to take exclusive control.

---

#### `sensors/drivers/ezo_sensor.c/h`
**Purpose**: Low-level Atlas Scientific EZO bus driver with convenience helpers per sensor family (RTD, pH, EC, DO, ORP, HUM).

**Responsibilities**:
- Issue commands (info, read, name, LED, calibration) over I2C with proper delays.
- Parse comma-separated responses into typed values and bubble errors up to the manager.
- Provide compensation hooks so EC/DO readings can leverage RTD temperature data.

**Key Functions**:
- `ezo_sensor_init()` / `_send_command()` / `_read_response()` - Base driver plumbing.
- `ezo_sensor_parse_measurement()` - Convert raw strings into floats per sensor type.

---

#### `sensors/drivers/max17048.c/h`
**Purpose**: MAX17048 Li-Ion fuel-gauge driver used for battery telemetry and health reporting.

**Responsibilities**:
- Initialize the IC, read version registers, and expose helpers for voltage/percentage reporting.
- Provide quick-look diagnostics for dashboard + MQTT payloads.

**Key Functions**:
- `max17048_init()` / `_read_voltage()` / `_read_percentage()` - Hardware accessors.
- `max17048_get_chip_version()` - Useful for logs and troubleshooting.

---

#### `services/core`
**Purpose**: Provide the single orchestration surface that boots all network/cloud services once Wi-Fi is online.

**Responsibilities**:
- Hold shared `services_config_t` / `services_dependencies_t` structs so HTTP, MQTT, mDNS, time sync, and key management operate behind one API.
- Launch mDNS + HTTPS dashboard services (and, soon, MQTT/time-sync helpers) once TLS assets and Wi-Fi readiness prerequisites are satisfied, while updating the boot event group bit on success.
- Offer `services_start()` / `services_stop()` entry points plus a status-listener callback so readiness/degradation signals bubble up to the boot coordinator without direct event-group manipulation from each service.
- Serve as the public include for the entire `services/` namespace; feature-specific directories (`http/`, `telemetry/`, etc.) stay encapsulated.

**Key Functions**:
- `services_start()` – Accepts configuration + dependencies, spins up mDNS + HTTPS (respecting TLS/time-sync guards), sets the boot ready bit, and emits READY/DEGRADED events for both the core and each component.
- `services_stop()` – Central stop hook that tears everything down, shuts down HTTP/mDNS if they were running, and emits STOPPED events per component plus a core-level STOPPED signal.

---

#### `services/http`
**Purpose**: Serve the on-device dashboard over HTTPS, expose REST control endpoints, stream real-time sensor data via WebSocket, and manage the editable web asset volume.

**Responsibilities**:
- `http_server.c/.h` boot the HTTPS server (skipped on ESP32-C6), register routes, and bridge into the provisioning/sensor subsystems.
- `http_handlers_sensors.c/.h` implement REST helpers for sensor configuration, calibration, and runtime operations without leaking driver internals.
- `http_websocket.c/.h` push sensor snapshots + focus streams to live dashboards via `/ws/sensors`.
- `web_file_editor.c/.h` mount the FATFS-backed `/www` partition, seed defaults, and expose helpers so the dashboard files can be patched over HTTP.

**Key Functions**:
- `http_server_start(const http_server_config_t *)` / `_stop()` – Lifecycle hooks invoked by `services_start()` once TLS assets are in place.
- `http_handlers_sensors_init()` – Prepares shared mutex/guards so sensor commands can pause the pipeline safely.
- `sensor_ws_handler()` / `http_websocket_snapshot_handler()` – Broadcast sensor cache updates to dashboard clients.
- `web_editor_init_fs()` / `_load_file()` – Guarantee the writable dashboard assets exist before HTTP starts.

---

## Configuration Files (`config/`)

### `sdkconfig.defaults`
Default ESP-IDF configuration settings:

```makefile
# Bluetooth configuration
CONFIG_BT_ENABLED=y
CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y

# WiFi configuration
CONFIG_ESP_WIFI_AUTH_WPA2_PSK=y

# Flash size
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y

# Partition table
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="config/partitions.csv"

# Log levels
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_LOG_MAXIMUM_LEVEL_VERBOSE=y

# ESP32-S3 specific
CONFIG_IDF_TARGET="esp32s3"
```

### `partitions.csv`
Flash memory partition layout:

```csv
# Name,      Type,    SubType, Offset,  Size,    Flags
nvs,         data,    nvs,     0x9000,  0x6000,  
phy_init,    data,    phy,     0xf000,  0x1000,  
factory,     app,     factory, 0x10000, 0x300000,
```

**Partitions**:
- **nvs** (24 KB): Non-Volatile Storage for WiFi credentials
- **phy_init** (4 KB): RF calibration data
- **factory** (3 MB): Application firmware

### `runtime/services.json`
Embedded JSON file consumed by `config/runtime_config.c` at boot. The contents are
compiled into the firmware via `EMBED_TXTFILES`, so editing the file requires a
rebuild. Supported fields:

```json
{
  "services": {
    "enable_http_server": true,
    "enable_mqtt": true,
    "enable_mdns": true,
    "enable_time_sync": true,
    "https_port": 443
  },
  "mqtt": {
    "broker_uri": "mqtts://mqtt.kannacloud.com:8883",
    "username": "sensor01",
    "password": "xkKKYQWxiT83Ni3",
    "publish_interval_sec": 0
  },
  "dashboard": {
    "mdns_hostname": "kc",
    "mdns_instance_name": "KannaCloud Device"
  },
  "time_sync": {
    "timezone": "UTC",
    "timeout_sec": 10
  }
}
```

`runtime_config_apply_services_overrides()` parses the JSON once, caches the
results, and mutates `services_config_t` before the services core launches. Any
missing fields fall back to the defaults in `services/core/services_config.c`.

### `runtime/api_keys.json`
Companion JSON file embedded next to `services.json`. Parsed by
`services/keys/api_key_manager.c` on boot; if encrypted NVS does not already hold
API keys, the manager seeds entries from this file and stores them securely.

```json
{
  "api_keys": [
    {
      "name": "Cloud Provisioning Key",
      "type": "cloud",
      "value": "REPLACE_WITH_PROVISIONING_KEY",
      "enabled": true
    }
  ]
}
```

Supported `type` values: `cloud`, `dashboard`, or `custom` (default). Keep the
checked-in value as a placeholder—real keys should be supplied in private builds
so they enter NVS during the first boot.

---

## Documentation (`docs/`)

### `README.md`
Complete project documentation including:
- Features overview
- Architecture explanation
- BLE service structure
- Provisioning states and status codes
- Build and flash instructions
- Usage guide (first boot vs. subsequent boots)
- Mobile app development guide with Kotlin examples
- Security considerations
- Troubleshooting
- Power consumption estimates
- Future enhancements

### `KOTLIN_INTEGRATION.md`
Quick reference for Android developers:
- UUIDs (service and characteristics)
- Device name
- JSON format for credentials
- JSON format for status notifications
- Complete provisioning flow example in Kotlin
- State/status code mapping table
- Testing checklist
- Troubleshooting specific to mobile integration

### `PROJECT_STRUCTURE.md`
This document - explains file organization and responsibilities.

---

## Test Files (`test/`)

### `test_credentials.json`
Sample WiFi credentials for testing:
```json
{
  "ssid": "MyHomeNetwork",
  "password": "SecurePassword123"
}
```

### `test_credentials_examples.txt`
Multiple credential format examples:
- Standard format with whitespace
- Minimal compact format
- Format with special characters in SSID/password

**Note**: These are for development only. Never commit real credentials.

### `test_provisioning_state_stub.py`
Host-side Python `unittest` skeleton that documents the provisioning state-machine
scenarios (`stored credentials`, `BLE happy path`, `auth failure`, `connection
guard`). Each test is currently skipped until the provisioning package exposes
mockable hooks, but contributors now have a canonical landing zone for future
assertions.

---

## Build Output (`build/`)

Generated by ESP-IDF build system. **Not tracked in Git.**

Key files (in both `build_s3/` and `build_c6/`):
- `kc_device.bin` - Application binary
- `kc_device.elf` - Executable with debug symbols
- `kc_device.map` - Memory map
- `bootloader/bootloader.bin` - Bootloader binary
- `partition_table/partition-table.bin` - Partition table binary
- `compile_commands.json` - For IDE integration

## Hardware Variants

### ESP32-S3
- **Architecture**: Xtensa dual-core (LX7)
- **CPU Frequency**: Up to 240 MHz
- **Wireless**: 2.4 GHz Wi-Fi 4, BLE 5.0
- **Build Directory**: `build_s3/`
- **Config File**: `sdkconfig.s3`
- **Flash Size**: 4MB (default)
- **Toolchain**: `xtensa-esp32s3-elf`

### ESP32-C6
- **Architecture**: RISC-V single-core (RV32IMAC)
- **CPU Frequency**: Up to 160 MHz
- **Wireless**: 2.4 GHz Wi-Fi 6 (802.11ax), BLE 5.0, Zigbee/Thread
- **Build Directory**: `build_c6/`
- **Config File**: `sdkconfig.c6`
- **Flash Size**: 2MB (default, adjustable)
- **Toolchain**: `riscv32-esp-elf`

Both variants share the same application code with chip-specific optimizations handled automatically by ESP-IDF.

---

## Development Workflow

### Using Build Script (Recommended)
```powershell
cd c:\Code\kc_device

# Build for ESP32-S3
.\build.ps1 -Target s3 -Action build

# Build for ESP32-C6
.\build.ps1 -Target c6 -Action build

# Build and flash ESP32-S3
.\build.ps1 -Target s3 -Action all -Port COM3

# Build and flash ESP32-C6
.\build.ps1 -Target c6 -Action all -Port COM3

# Configuration menu
.\build.ps1 -Target s3 -Action menuconfig
.\build.ps1 -Target c6 -Action menuconfig

# Clean build
.\build.ps1 -Target s3 -Action fullclean
```

### Manual Build (Traditional Method)
```powershell
# ESP32-S3
idf.py -B build_s3 -D SDKCONFIG=sdkconfig.s3 build
idf.py -B build_s3 -D SDKCONFIG=sdkconfig.s3 -p COM3 flash monitor

# ESP32-C6
idf.py -B build_c6 -D SDKCONFIG=sdkconfig.c6 build
idf.py -B build_c6 -D SDKCONFIG=sdkconfig.c6 -p COM3 flash monitor
```

---

## Dependencies

### ESP-IDF Components
- **nvs_flash** - Non-Volatile Storage
- **bt** - Bluetooth stack (Bluedroid)
- **esp_wifi** - WiFi driver
- **json** - cJSON parser
- **esp_timer** - High-resolution timers
- **freertos** - Real-Time Operating System
- **esp_netif** - Network interface abstraction
- **esp_event** - Event loop system

### External Dependencies
None - all dependencies are provided by ESP-IDF.

---

## Code Metrics

| File | Lines | Purpose |
|------|-------|---------|
| `main/provisioning/wifi_manager.c` | 338 | WiFi + NVS |
| `main/main.c` | 170 | Application entry |
| `main/provisioning/provisioning_state.c` | 98 | State machine |
| **Total Source Code** | **~1,250** | |
| | | |
| `docs/README.md` | 323 | Main documentation |
| `docs/KOTLIN_INTEGRATION.md` | 201 | Integration guide |
| **Total Documentation** | **~617** | |

---

## Git Repository

**Repository**: `esp32_S3_wifi_provisioning_using_c_example`  
**Owner**: `d0773d`  
**Branch**: `main`

### `.gitignore`
Ignores:
- `build/` - Build artifacts
- `sdkconfig`, `sdkconfig.old` - Generated config
- `*.bin`, `*.elf`, `*.map` - Binary files
- `dependencies.lock`, `managed_components/` - Dependencies
- IDE files (`.vscode/`, `.idea/`)
- OS files (`.DS_Store`, `Thumbs.db`)
- `credentials.json`, `secrets.h` - Never commit real credentials

---

## Version Information

**Project Version**: 1.0.0  
**ESP-IDF Version**: 5.5.1-2  
**Target**: ESP32-S3  
**Date**: November 2025

---

## Quick Reference

### Directory Purposes

| Directory | Purpose | Tracked in Git |
|-----------|---------|----------------|
| `main/` | Application source code | ✅ Yes |
| `config/` | Configuration files | ✅ Yes |
| `docs/` | Documentation | ✅ Yes |
| `test/` | Test files and examples | ✅ Yes |
| `build/` | Build artifacts | ❌ No |

### File Extensions

- `.c` - C source code
- `.h` - C header files
- `.md` - Markdown documentation
- `.csv` - Partition table
- `.json` - Test data
- `.txt` - Plain text documentation

### Key Concepts

- **NVS**: Non-Volatile Storage - Flash memory storage for persistent data
- **GATT**: Generic Attribute Profile - BLE service/characteristic structure
- **MTU**: Maximum Transmission Unit - Maximum packet size (517 bytes)
- **CCCD**: Client Characteristic Configuration Descriptor - Enable/disable notifications
- **Bonding**: Secure pairing with key storage for reconnection

---

## Related Resources

- [ESP-IDF Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/)
- [ESP32 BLE Examples](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth)
- [Bluetooth Core Specification](https://www.bluetooth.com/specifications/specs/)

---

Last Updated: November 15, 2025
