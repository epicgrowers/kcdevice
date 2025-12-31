# KC-Device-ESP32-S3 – Detailed Overview

This document explains how the firmware provisions Wi-Fi using the ESP-IDF provisioning manager, how components interact, and how to operate or extend the system.

## 1. Provisioning Architecture

| Layer | File(s) | Responsibility |
|-------|---------|----------------|
| Provisioning runner | `main/provisioning/provisioning_runner.c/.h` | Owns the single `provisioning_run()` call: attempts stored credentials, launches BLE provisioning when required, reports outcomes, and exposes the reconnection guard used by `main.c`. |
| Provisioning manager wrapper | `main/provisioning/idf_provisioning.c/.h` | Starts/stops ESP-IDF's Wi-Fi provisioning manager, selects BLE transport, enforces Security 1 + PoP, bridges callbacks into the local provisioning state machine, and kicks off Wi-Fi connection attempts once credentials arrive. |
| Provisioning state machine | `main/provisioning/provisioning_state.c/.h` | Tracks transitions such as *BLE connected*, *credentials received*, *Wi-Fi connecting*, *provisioned*, or *failure*; notifies registered listeners (currently `main.c` logging). |
| Wi-Fi manager | `main/provisioning/wifi_manager.c/.h` | Owns the station interface, saves credentials to NVS, retries connections, and exposes helpers to query connection state or clear credentials. |
| Reset button | `main/platform/reset_button.c/.h` | Short press clears Wi-Fi credentials and restarts; long press performs a factory reset (NVS erase). |
| Cloud + services | `main/services/provisioning/cloud_provisioning.c`, `http_server.c`, `mqtt_telemetry.c`, `sensor_manager.c`, etc. | Idle until Wi-Fi is online, then perform their usual duties (TLS provisioning, HTTPS dashboard, MQTT telemetry, sensor polling). |

### ESP-IDF Provisioning Manager Configuration

- **Transport**: BLE (`wifi_prov_scheme_ble`)
- **Security**: Security 1 (X25519 key exchange + PoP)
- **Proof-of-Possession**: `sumppop`
- **Service name**: `kc-<MAC[3..5]>` (e.g., `kc-12ABCD`)
- **Event handlers**: `WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM` (frees BTDM after provisioning) and `WIFI_PROV_EVENT_HANDLER_NONE` for app-level callbacks.

### Flow

1. `main.c` boots, initializes security, the reset button, the provisioning state machine, and builds a `provisioning_plan_t` for the runner.
2. `provisioning_run()` initializes the Wi-Fi stack and tests stored credentials through the selected `provisioning_wifi_ops_t`. Success skips BLE provisioning entirely.
3. On failure/no creds, `idf_provisioning_start()` starts the provisioning manager using the same Wi-Fi ops implementation (and now refuses to run if the ops init hook did not bring up the Wi-Fi stack). BLE advertisements become visible in the ESP BLE Provisioning app while sensors continue booting.
4. User selects Security 1, enters the PoP `sumppop`, and supplies SSID/password.
5. ESP-IDF provisioning manager emits `WIFI_PROV_CRED_RECV`; the wrapper hands those credentials to the Wi-Fi ops implementation so the same code path (default: `wifi_manager`) drives the connection.
6. When the Wi-Fi ops implementation reports `IP_EVENT_STA_GOT_IP`, credentials are saved and provisioning stops (`idf_provisioning_stop()`), freeing BLE resources and seeding the connection guard cache.
7. Cloud services start (`network_boot_start()`), while `provisioning_connection_guard_poll()` keeps Wi-Fi healthy during normal operation.

## 2. Provisioning State Machine

```mermaid
graph LR
    A[IDLE] -->|BLE active| B[BLE_CONNECTED]
    B -->|Credentials arrived| C[CREDENTIALS_RECEIVED]
    C --> D[WIFI_CONNECTING]
    D -->|DHCP success| E[PROVISIONED]
    D -->|Auth/timeout failure| F[WIFI_FAILED]
    F -->|Retries exhausted| B
```

- All transitions call `provisioning_state_set(state, status_code, message)`
- `main.c` registers `state_change_handler()` for logging/UI bridging
- State codes are shared with the mobile client documentation (see `docs/KOTLIN_INTEGRATION.md`)

## 3. Storage & Partitions

- `config/partitions.csv` defines two OTA slots sized at `0x1B0000` (≈1.73 MB) each, providing headroom for the current 1.66 MB binary
- FATFS (`www` partition backed by wear-levelling) starts at `0x380000` with a size of `0x080000`
- Wi-Fi credentials live in the `wifi_config` NVS namespace; they are cleared via the reset button or programmatically by `wifi_manager_clear_credentials()`

## 4. Operation Checklist

1. **Build / Flash / Monitor**
   ```powershell
   idf.py set-target esp32s3
   idf.py build flash monitor
   ```
2. **Provision**
   - Open the Espressif "ESP BLE Provisioning" mobile app
   - Scan for `kc-XXXXXX`
   - Choose *BLE*, *Security 1*, PoP `sumppop`
   - Enter Wi-Fi SSID/password and wait for "Provisioned" confirmation
3. **Clear Credentials**
   - Short press GPIO0 (BOOT) to erase Wi-Fi credentials and reboot into provisioning mode
   - Long press for full factory reset

## 5. Extending the System

- **Custom mobile client**: Follow `docs/KOTLIN_INTEGRATION.md` for implementing Security 1 provisioning in Android/Kotlin
- **Alternate PoP**: Update `kPop` in `main/provisioning/idf_provisioning.c` and keep the secret synchronized with your provisioning UI
- **SoftAP provisioning**: The wrapper isolates BLE-specific pieces; swapping to `wifi_prov_scheme_softap` only requires updating the config struct and ensuring menuconfig enables SoftAP transport

## 6. Logs & Troubleshooting

- `idf_prov`: lifecycle of the provisioning manager wrapper (start/stop, service name, warnings)
- `wifi_prov_mgr`: lower-level provisioning manager logs (credential events, failures)
- `wifi_manager`: Wi-Fi connection attempts, retries, and NVS saves
- Common failure causes:
  - Wrong PoP → `WIFI_PROV_CRED_FAIL`
  - Bad password / missing SSID → `STATUS_ERROR_WIFI_AUTH_FAILED`
  - Provisioning skipped because stored creds succeeded → BLE not advertising; clear credentials first

## 7. Related Documents

- `KOTLIN_INTEGRATION.md` – App flow and Retrofit helpers for provisioning, retrieving state, etc.
- `PROJECT_STRUCTURE.md` – File-by-file explanation of the repo
- `SECURITY.md` – Secure boot, flash encryption, NVS encryption, PoP rotation tips
- `WEB_EDITOR_QUICKSTART.md` / `WEB_FILE_EDITOR.md` – HTTPS dashboard customization guides

This documentation intentionally omits the legacy custom GATT service; all provisioning functionality now routes through the ESP-IDF provisioning manager.

## 8. Runtime Config & Key Rotation

Follow this checklist whenever you need to change environment-specific settings or rotate secrets:

1. **Edit runtime config**: Update `config/runtime/services.json` for feature toggles (HTTP, MQTT, mDNS, time-sync) and `config/runtime/api_keys.json` for dashboard/cloud API keys. Never commit production secrets; keep the checked-in file with placeholders and apply real values in your private branch.
2. **Rebuild the firmware**: `build.ps1 -Target s3 -Action build` (or `-Target c6`) embeds the JSON blobs via `EMBED_TXTFILES`. A rebuild is required every time either file changes.
3. **Clear stale secrets**: Long-press the BOOT button for a factory reset or run `idf.py erase_flash` so encrypted NVS is wiped. On next boot the API key manager reseeds from the embedded JSON before cloud provisioning runs.
4. **Flash + verify**: Flash the new binary, open the monitor, and confirm that `API_KEY_MGR` logs “seeded from runtime config” before services start. Provision Wi-Fi if needed, then ensure HTTPS/MQTT start without “Cloud API key unavailable” errors.

### Runtime validation rules

`build.ps1` now fails fast if the embedded JSON blobs are missing required fields for the services you enabled:


This flow keeps secrets in a single encrypted store (NVS) while still allowing per-environment overrides without code changes.

### Detecting config drift


## 9. Developer Utilities & Host Tests

- **Host harness**: All Python-based provisioning tests live under `test/host/`. Run them with `python -m unittest discover -s test/host -p "test_*.py"`. The suite uses `provisioning_sim.py` to mirror the provisioning state machine, so we already cover stored-credential reuse, BLE success paths, Wi-Fi auth failures, and simulated reconnection-guard behavior without hardware.
- **Runtime-config hashing**: `tools/generate_runtime_config_hash.py` consumes `config/runtime/services.json` and `config/runtime/api_keys.json`, emits `runtime_config_hash.h`, and runs automatically during `idf.py build` / `build.ps1`. Invoke it directly if you need to inspect the generated SHA256 digests while debugging OTA workflows.
- **Runtime-config validator**: `tools/validate_runtime_config.py` mirrors the lint rules enforced by `build.ps1`. Run `python tools/validate_runtime_config.py` (optionally with `--services` / `--api-keys` overrides) to confirm JSON edits satisfy HTTPS, MQTT, dashboard, time-sync, and API-key requirements before kicking off an ESP-IDF build.
- Firmware exposes those digests through `runtime_config_get_digest()`, allowing OTA workflows or diagnostics endpoints to compare the embedded JSON against what the device reports at runtime.
