# KC-Device (ESP32-S3 / ESP32-C6)

Secure Wi-Fi provisioning firmware for ESP32-S3 and ESP32-C6 chips based on the ESP-IDF v5.5 provisioning manager. The firmware exposes Espressif's standard provisioning service over BLE (Security 1 with Proof-of-Possession) and falls back to the new provisioning flow automatically whenever stored Wi-Fi credentials are missing or invalid.

## Supported Hardware

- **ESP32-S3** - Xtensa dual-core, 2.4 GHz Wi-Fi, BLE 5.0
- **ESP32-C6** - RISC-V single-core, 2.4 GHz Wi-Fi 6, BLE 5.0, Zigbee/Thread

## Highlights

- **Provisioning Runner** (`main/provisioning/provisioning_runner.c/.h`)
  - Attempts stored-credential reconnection before falling back to BLE provisioning
  - Launches sensor boot work while the user interacts with the app
  - Exposes `provisioning_run()` and the reconnection guard so `app_main()` remains orchestration-only
- **ESP-IDF Provisioning Manager** (`main/provisioning/idf_provisioning.c/.h`)
  - BLE transport with Espressif's provisioning schemas and characteristic layout handled by the framework
  - Security 1 (X25519 + PoP) handshake with shared password `sumppop`
  - Automatic teardown of BLE resources when Wi-Fi connects
- **Wi-Fi Manager** (`main/provisioning/wifi_manager.c/.h`)
  - Stores credentials securely in NVS after a successful DHCP lease
  - Handles reconnect logic, retry timers, and credential clearing
- **Provisioning State Machine** (`main/provisioning/provisioning_state.c/.h`)
  - Centralized event callbacks for logging/UI updates
  - Shared enum/status codes consumed by the provisioning manager and Wi-Fi stack
- **Reset Button Integration** (`main/reset_button.c/.h`)
  - Short press: clear Wi-Fi credentials and reboot to provisioning mode
  - Long press: full NVS erase / factory reset
- **Cloud + Peripheral Stack**
  - Time sync, HTTPS dashboard, MQTT telemetry, sensor polling, and API key manager remain unchanged and simply wait for `wifi_manager` to report a connection

## Project Layout

```
main/
  provisioning/
    provisioning_runner.c/.h  # Stored creds + BLE provisioning surface
    idf_provisioning.c/.h     # ESP-IDF provisioning manager wrapper
    provisioning_state.c/.h   # Shared provisioning state machine
    wifi_manager.c/.h         # Wi-Fi + NVS logic
  reset_button.c/.h           # GPIO0 short/long press handling
  ...                         # Cloud, HTTP, MQTT, sensors, etc.
config/
  partitions.csv          # Enlarged OTA slots (0x1B0000 each)
  sdkconfig.defaults      # ESP32-S3 default config
  sdkconfig.esp32c6       # ESP32-C6 default config
sdkconfig.s3              # ESP32-S3 active configuration
sdkconfig.c6              # ESP32-C6 active configuration
build.ps1                 # Build script for easy target switching
```

## Quick Start

### Using the Build Script (Recommended)

```powershell
# For ESP32-S3
.\build.ps1 -Target s3 -Action all -Port COM3

# For ESP32-C6
.\build.ps1 -Target c6 -Action all -Port COM3

# Other commands
.\build.ps1 -Target s3 -Action build          # Build only
.\build.ps1 -Target c6 -Action flash -Port COM3   # Flash only
.\build.ps1 -Target s3 -Action monitor -Port COM3 # Monitor only
.\build.ps1 -Target c6 -Action menuconfig     # Configuration menu
```

### Manual Build (Traditional Method)

```powershell
# For ESP32-S3
cd C:\Code\kc_device
idf.py -B build_s3 -D SDKCONFIG=sdkconfig.s3 build flash monitor -p COM3

# For ESP32-C6
idf.py -B build_c6 -D SDKCONFIG=sdkconfig.c6 build flash monitor -p COM3
```

Key runtime facts:

- BLE advertisement name: `kc-<last-3-bytes-of-MAC>` (e.g., `kc-12ABCD`)
- Transport: BLE, Security 1, PoP = `sumppop`
- Provision using the Espressif "ESP BLE Provisioning" Android/iOS app
- Stored credentials skip provisioning; clear them with a short press on GPIO0

## Provisioning Flow

1. `provisioning_run()` initializes `wifi_manager` and attempts stored credentials (if configured)
2. Failure or no credentials -> `idf_provisioning_start()` launches the ESP-IDF provisioning manager
3. User connects via app, enters PoP, and sends SSID/password
4. `wifi_manager_connect()`/ESP-IDF provisioning flow applies creds, saves them to NVS upon DHCP success, and tears down BLE
5. `provisioning_connection_guard_poll()` keeps Wi-Fi healthy while the rest of the system (cloud/time-sync/HTTPS/MQTT) waits on readiness bits

## Maintenance Notes

- The legacy `ble_provisioning.c/.h` implementation and its custom UUID mapping have been removed. All provisioning now routes through `provisioning/idf_provisioning.c`.
- If firmware size grows again, adjust `config/partitions.csv`; OTA slots currently provide ~64 KB of headroom over the latest build.
- Use `idf.py monitor` to view provisioning logs (`idf_prov`, `wifi_prov_mgr`, `wifi_manager`). Security failures will show up as `WIFI_PROV_CRED_FAIL` events.

## Documentation

- `docs/README.md` – detailed architecture overview, provisioning flow, and setup notes
- `docs/KOTLIN_INTEGRATION.md` – Android client details
- `docs/PROJECT_STRUCTURE.md` – component-level breakdown
- `docs/SECURITY.md` – device security posture and hardening tips

## License

MIT – see repository headers for details.
