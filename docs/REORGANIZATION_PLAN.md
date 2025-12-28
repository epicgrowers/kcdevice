# KC-Device Reorganization Plan

This document tracks the ongoing effort to reshape the KC-Device firmware into clearer, composable modules. It captures what has already landed, the architectural principles that will guide future edits, and the work chunks that should be tackled one segment at a time.

## Current Progress

- Split Wi-Fi bring-up into the `provisioning/` package (runner + shared config helpers), isolating stored-credential retries and provisioning orchestration from `main/main.c`.
- Introduced provisioning callbacks that let sensor tasks start while BLE provisioning runs, keeping the boot flow non-blocking.
- Added `network/network_boot.c` and `sensors/sensor_boot.c` launch helpers plus shared event-group bits so parallel tasks can report readiness consistently.
- Hardened the security and reset path by centralizing `security_init()` and reset-button handling near the top of `app_main()`.
- Verified the refactor in hardware: the new flow boots, provisions, and reconnects cleanly on ESP32-S3.
- Introduced `boot/boot_coordinator.c` to own the boot event group, sensor launch callbacks, and wait helpers, shrinking `main/main.c` back toward pure orchestration.
- Added `sensors/pipeline.{c,h}` and rewired the boot coordinator/main flow to launch sensors exclusively through the pipeline context so provisioning and stored-credential boots share the same entry point.
- Extracted provisioning/logging callbacks to `boot/boot_handlers.*` and trimmed `main/main.c` to 184 lines so it only wires modules together.
- Added `provisioning_run()`/`provisioning_connection_guard_poll()` so all Wi-Fi lifecycle logic lives behind the provisioning package and documentation now reflects the new directory layout.
- Moved the mDNS service and API key manager into `services/discovery/` and `services/keys/` so every Segment 4 subtree now lives under the services namespace alongside the existing HTTP, telemetry, and time-sync modules.
- Created `config/runtime/` with an embedded `services.json` plus a `runtime_config` loader so network boot consumes environment-specific overrides before launching the services core.
- Relocated `wifi_manager.c/.h` into the provisioning package (`main/provisioning/`) so the entire stored-credential and reconnection flow stays encapsulated with the rest of the provisioning stack.
- Routed TLS certificates, MQTT CA blobs, and device metadata through a new `services_provisioning_api_t` so only `network_boot.c` touches `cloud_provisioning`, while HTTP + MQTT modules receive everything via `services_config_t`.

## Guiding Principles

1. **Component boundaries first**: Move code that represents a service (Wi-Fi, sensors, HTTP, MQTT, OTA) into focused directories with their own headers and CMake targets.
2. **Single orchestration surface**: Keep `app_main()` limited to wiring event groups, launching subsystems, and reacting to high-level state—not business logic.
3. **Configuration lives in one place**: Ensure runtime constants (GPIOs, stack sizes, provisioning limits) are defined under `boot/` or `config/`, never sprinkled through feature modules.
4. **Async-safe boot graph**: Treat each boot phase (security, storage, connectivity, services, telemetry) as idempotent tasks that can be retried or delayed without stalling unrelated work.
5. **Docs track reality**: Update `docs/PROJECT_STRUCTURE.md` and this plan whenever paths or responsibilities change so onboarding stays painless.

## Work Segments

### Segment 1 – Boot Coordination Stabilization

**Goal**: Give all boot-phase code (`boot/`, `network/`, `sensors/`) a single owner module and shrink `app_main()` to orchestration only.

_Status 2025-12-26_: **Completed** – boot coordinator + handlers own all boot glue, `main/main.c` is 184 lines of orchestration, and `docs/BOOT_ARCHITECTURE.md` now documents the coordinator timeline.

**Tasks**:
- Move the event-group creation plus helper bits into `boot/boot_coordinator.c` and expose a minimal API for setting/awaiting readiness flags.
- Relocate `launch_sensors_during_provisioning()` and similar helpers from `main/main.c` into their respective boot modules.
- Document the boot timeline (Phase 0–4) in `docs/BOOT_ARCHITECTURE.md` using the new component names.

**Deliverables**:
- `main/main.c` under ~200 lines with no direct sensor/network logic.
- Updated boot diagram + README snippet referencing the coordinator module.

### Segment 2 – Provisioning + Wi-Fi Stack Cleanup

_Status 2025-12-27_: **In progress** – provisioning sources live under `main/provisioning/`, `provisioning_run()` replaces the old helper, the reconnection guard moved out of `main.c`, and the Wi-Fi manager is now encapsulated behind provisioning helpers. Host-side state-transition stubs now live in `test/test_provisioning_state_stub.py` while we finish wiring the harness.

**Goal**: Merge provisioning state, Wi-Fi manager, and the provisioning runner into a cohesive package that hides internal event chatter.

**Tasks**:
- Create `provisioning/` submodule containing `idf_provisioning.*`, `provisioning_state.*`, and shared logging helpers.
- Expose a single `provisioning_run(const provisioning_plan_t *plan, provisioning_outcome_t *out)` entry point that wraps BLE start, credential receipt, Wi-Fi connection, and teardown.
- Strip remaining `wifi_manager_*` calls from outside the provisioning package except for health checks.

**Deliverables**:
- Public header documenting the provisioning plan/outcome structures.
- Unit-test stubs (can be host-side) for state transitions (initial skeleton: `test/test_provisioning_state_stub.py`).

### Segment 3 – Sensor Stack Packaging

_Status 2025-12-27_: **Completed** – sensor drivers, boot helpers, and the new pipeline module now live under `main/sensors/` with shared config + telemetry surfaces wired through to MQTT/HTTP.

**Goal**: Turn individual sensor drivers (EZO, MAX17048, etc.) plus `sensor_manager` into a reusable `sensors/` component with clear dependency injection.

**Delivered**:
- Drivers now sit under `sensors/drivers/` and the boot layer talks exclusively to `sensors/pipeline.c`.
- `sensors/config.{c,h}` owns the discovery table plus stabilization/retry timing so no other module hard-codes I2C constants.
- `sensor_pipeline_snapshot_t` and listener APIs let MQTT/HTTP modules consume readiness + cached telemetry without touching `sensor_manager` internals.
- Docs updated (`BOOT_ARCHITECTURE`, this plan) to describe pipeline + discovery responsibilities and the include tree is now `boot -> sensors/pipeline.h -> drivers/...`.

### Segment 4 – Network + Cloud Services Module

**Goal**: Consolidate HTTP, MQTT, API keys, mDNS, and time sync under a `services/` (or `cloud/`) namespace with lifecycle hooks.

_Status 2025-12-27_: **Completed** – `services_config_t` now carries a provisioning/provider interface so HTTP + MQTT load TLS assets without touching `cloud_provisioning`, and `services_start()` drives every service plus the degraded/fault callbacks.

**Tasks**:
- [done] Carve out a `services/` tree:
	- `services/core/` (new `services.h`, `services_start()/services_stop()`, config structs, readiness bits)
	- `services/http/` (existing `http_server`, `http_handlers_sensors`, `http_websocket`, `web_file_editor`)
	- `services/telemetry/` (`mqtt_telemetry` and future telemetry workers)
	- `services/discovery/` (`mdns_service`, future DNS-SD helpers)
	- `services/time_sync/` (`time_sync.c`)
	- `services/keys/` (`api_key_manager.c`)
	- `services/provisioning/` (`cloud_provisioning.c` for TLS assets and device metadata)
- [done] Move the HTTP server, handlers, WebSocket, and web file editor sources underneath `services/http/` and update the build/doc references.
- [done] Define `services_config_t` with TLS cert refs, MQTT interval, dashboard enable flags, and pass it from the boot flow (currently `network_boot.c`). The new `services_provisioning_api_t` wraps `cloud_provisioning` so modules receive TLS/device metadata via config instead of direct includes.
- [done] Implement `services_start(const services_config_t *, const services_dependencies_t *, services_status_listener_t cb)` that launches HTTP/MQTT/time sync once Wi-Fi ready and updates the boot event group (HTTP + mDNS now run through `services_start()`). HTTP + MQTT now receive typed config structs and refuse to start if provisioning hooks are missing.
- [done] Add degradation callbacks so failures bubble to boot coordinator for retries or safe-mode decisions (`services_fault_handler_t` latches `NETWORK_DEGRADED_BIT` and publishes `network_fault`).

**Deliverables**:
- `services/README.md` explaining responsibilities + dependencies.
- Event-bit diagram covering service readiness and degradation modes.

### Segment 5 – Configuration + Secrets Hygiene

_Status 2025-12-27_: **In progress** – runtime overrides now live in `config/runtime/services.json`, `config/runtime/api_keys.json` seeds the encrypted secret store, and `network_boot.c` applies overrides at boot while we finish build validation.

**Goal**: Untangle configuration values from source files and ensure secrets are pluggable per environment.

**Tasks**:
- [done] Create `config/runtime/` for JSON/TOML describing sensors, endpoints, certificates, etc., and load them at boot.
- [done] Ensure `api_key_manager` pulls from a single secret store (encrypted NVS seeded via `config/runtime/api_keys.json`) instead of hard-coded constants.
- Expand `build.ps1` to validate required configs (e.g., fail early if certificates are missing for HTTPS dashboard).

**Deliverables**:
- Checklist in `docs/README.md` describing how to rotate keys and provision configs.
- Optional CMake target that embeds config blobs with hashing so OTA updates can detect drift.

## Tracking + Next Steps

1. Log progress in this file after each segment completes (what landed, links to PRs/commits).
2. Keep `docs/PROJECT_STRUCTURE.md` synchronized with directory moves to avoid stale onboarding material.
3. Whenever a segment introduces new build targets, update `build.ps1` and mention them in `README.md`.
4. Consider tagging releases between major segments so QA can bisect regressions quickly.

_Last updated: 2025-12-27_
