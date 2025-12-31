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
- Added a `runtime_config_hash` build target plus `runtime_config_get_digest()` so every firmware image now carries SHA256 digests + sizes for the embedded JSON blobs, letting OTA updates confirm config drift before launching services.
- Added `provisioning_wifi_ops_t` so `provisioning_run()` can swap Wi-Fi implementations (or host-side stubs) without leaking `wifi_manager_*` past the provisioning package.
- Taught `idf_provisioning_start()` to accept the injected Wi-Fi ops pointer so BLE credential events now travel through the same `wifi_manager` (or test stub) path as stored-credential boots.
- Removed duplicate `esp_netif_init()`/`esp_wifi_init()` inside `idf_provisioning_start()` so Wi-Fi lifecycle ownership fully lives inside the injected ops and provisioning now fails fast when the caller forgets to initialize Wi-Fi.
- Consolidated `chip_info`, `security`, `reset_button`, and `i2c_scanner` under a new `platform/` directory so board-specific glue no longer clutters the component root and future hardware variants have a single home for overrides.
- Introduced `test/host/` with a README plus the `provisioning_sim.py` harness so provisioning state-machine tests now run (covering stored credentials, BLE success, auth failures, and reconnection guard scenarios).
- Documented the `tools/` directory (currently `generate_runtime_config_hash.py`) so contributors know where developer utilities live and how the runtime-config hashing step works.
- Added `tools/validate_runtime_config.py` so runtime JSON edits can be linted locally, preventing slow ESP-IDF builds from failing on schema mistakes.
- Began carving `services/telemetry/mqtt_telemetry.c` by moving JSON/payload construction into `mqtt_payload_builder.c`, paving the way for additional telemetry refactors.
- Introduced `services/telemetry/mqtt_connection_controller.c` to own MQTT supervisor state so `mqtt_telemetry.c` can focus on payload publishing logic.
- Extended the controller with a metrics snapshot API (state, run flag, reconnect count, consecutive failures, and last transition timestamp) so future dashboards/tests can introspect telemetry health without reaching into the publish task.
- Added an HTTPS endpoint (`/api/mqtt/status`) that exposes those metrics for dashboards, CLI diagnostics, or host automation without requiring firmware changes.
- Added `config/runtime/sensor_profiles.json` plus `tools/validate_sensor_profiles.py` to lint Atlas sensor stacks (duplicate I2C addresses, unsupported channels, etc.) outside of ESP-IDF builds.
- Added telemetry tuning knobs (publish interval + busy/client/idle backoffs) to `config/runtime/services.json`, wiring them through `runtime_config`, `services_config_t`, `mqtt_telemetry.c`, `tools/validate_runtime_config.py`, and the build script so publish cadence is environment-configurable without recompiling.

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

_Status 2025-12-30_: **Completed** – provisioning sources live under `main/provisioning/`, `provisioning_run()` replaces the old helper, the reconnection guard moved out of `main.c`, `idf_provisioning_start()` now feeds credentials through the injected Wi-Fi ops (default: `wifi_manager`), and host-side state-transition stubs live in `test/host/test_provisioning_state_stub.py` for future harnesses.

**Goal**: Merge provisioning state, Wi-Fi manager, and the provisioning runner into a cohesive package that hides internal event chatter.

**Tasks**:
- Create `provisioning/` submodule containing `idf_provisioning.*`, `provisioning_state.*`, and shared logging helpers.
- Expose a single `provisioning_run(const provisioning_plan_t *plan, provisioning_outcome_t *out)` entry point that wraps BLE start, credential receipt, Wi-Fi connection, and teardown.
- Strip remaining `wifi_manager_*` calls from outside the provisioning package except for health checks.

**Deliverables**:
- Public header documenting the provisioning plan/outcome structures.
- Unit-test stubs (can be host-side) for state transitions (initial skeleton: `test/host/test_provisioning_state_stub.py`).

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

_Status 2025-12-30_: **Completed** – runtime overrides now live in `config/runtime/services.json`, encrypted API keys seed from `config/runtime/api_keys.json`, `build.ps1` enforces schema validation, and the new `runtime_config_hash` target embeds SHA256 digests exposed via `runtime_config_get_digest()` so OTA upgrades can detect drift.

**Goal**: Untangle configuration values from source files and ensure secrets are pluggable per environment.

**Tasks**:
- [done] Create `config/runtime/` for JSON/TOML describing sensors, endpoints, certificates, etc., and load them at boot.
- [done] Ensure `api_key_manager` pulls from a single secret store (encrypted NVS seeded via `config/runtime/api_keys.json`) instead of hard-coded constants.
- [done] Expand `build.ps1` to validate required configs (e.g., fail early if certificates are missing for HTTPS dashboard).
- [done] Generate SHA256 digests for the embedded JSON blobs via `runtime_config_hash` (run with `idf.py runtime_config_hash`) and expose them at runtime for OTA verification.

**Deliverables**:
- Checklist in `docs/README.md` describing how to rotate keys and provision configs.
- Optional CMake target that embeds config blobs with hashing so OTA updates can detect drift.

### Segment 6 – Platform Utilities Consolidation

_Status 2025-12-30_: **Completed** – chip metadata logging, security init, reset-button glue, and the I2C scanner now live under `main/platform/`, `main.c` only includes `platform/*` headers, and `docs/PROJECT_STRUCTURE.md` reflects the new ownership boundary.

**Goal**: Group the hardware-adjacent helpers that were cluttering the component root so future board variants (or host harnesses) can override them in one place.

**Tasks**:
- Create `main/platform/` and move `chip_info.*`, `security.*`, `reset_button.*`, and `i2c_scanner.*` into it without behavioral changes.
- Update all includes (`main.c`, `boot_handlers`, sensor sources) to reference `platform/...` paths.
- Point `main/CMakeLists.txt` at the new file locations and keep the runtime config hash integration intact.
- Refresh `docs/PROJECT_STRUCTURE.md` (and this plan) to introduce the `platform/` section so onboarding material matches the codebase.

**Deliverables**:
- Clean `main/` root that only exposes orchestration + submodules.
- Documented `platform/` responsibilities for future contributors.

### Segment 7 – Developer Tooling & Host Harness

_Status 2025-12-31_: **Completed** – `test/host/` now contains the provisioning simulator + active `unittest` coverage (stored credentials, BLE happy path, auth failure, reconnection guard), `docs/PROJECT_STRUCTURE.md` / `docs/README.md` describe the host harness plus runtime-config tooling, `services/telemetry/mqtt_connection_controller.c` owns connection/retry state outside `mqtt_telemetry.c` and now exposes metrics snapshots (state/run flag/reconnect counters/timestamps), `tools/validate_sensor_profiles.py` lints the new `config/runtime/sensor_profiles.json`, and the latest evaluation confirmed the MQTT helper split + host tooling backlog are sufficient for the current telemetry scope.

**Goal**: Turn the “future cleanups” list into actionable deliverables by organizing host-side tests and documenting the tooling required to validate runtime configs outside the firmware build.

**Tasks**:
- [done] Create `test/host/`, move `test_provisioning_state_stub.py`, and add a README documenting the execution command + coverage roadmap.
- [done] Flesh out provisioning mocks via `provisioning_sim.py` so the unit tests assert stored-credential reuse, BLE happy paths, Wi-Fi failure codes, and connection-guard retries without hardware.
- [done] Add documentation under `tools/` plus the `validate_runtime_config.py` CLI so runtime-config linting can happen without invoking ESP-IDF.
- [done] Extract MQTT payload construction into `services/telemetry/mqtt_payload_builder.c` so the main telemetry loop focuses on connection + scheduling.
- [done] Introduce `services/telemetry/mqtt_connection_controller.c` to encapsulate supervisor/retry state while `mqtt_telemetry.c` manages payloads.
- [done] Add `config/runtime/sensor_profiles.json` plus `tools/validate_sensor_profiles.py` so Atlas sensor stacks can be linted locally before flashing.
- [done] Teach the connection controller to expose metrics snapshots (state, should-run flag, reconnect count, consecutive failures, last transition timestamps) for future dashboards and host tests.
- [done] Wire `/api/mqtt/status` into the HTTPS server so those metrics are available to dashboards/scripts without decoding logs.
- [done] Add `tools/mqtt_status_probe.py` so developers can poll `/api/mqtt/status` from a laptop CLI when diagnosing MQTT issues.
- [done] Add `tools/live_diag_report.py` for combined `/api/status` + `/api/mqtt/status` polling to provide live diagnostic summaries from the host.
- [done] Add `tools/ws_diag.py` for `/ws/sensors` monitoring (WebSocket focus snapshots/status) so developers can confirm streaming telemetry end-to-end.
- [done] Add a lightweight `mqtt_connection_watchdog` helper (timer task + fault hook) that inspects `mqtt_connection_controller_get_metrics()` and raises a services degradation event when the client is stuck in CONNECTING/ERROR for longer than a configurable window.
- [done] Split the publish loop into a dedicated scheduler helper (`mqtt_publish_loop.c`) so the FreeRTOS task now delegates interval/back-pressure logic to a testable unit that mirrors publish-on-read triggers and offline backoffs.
- [done] Extend the host harness with `test/host/mqtt_watchdog_sim.py` + tests to simulate MQTT disconnect storms, validating watchdog degradation events and the `/api/mqtt/status` payload without hardware.
- Continue evaluating whether additional MQTT helpers (connection monitors vs. publisher tasks) or host harnesses are needed as telemetry grows.

**Deliverables**:
- Green (non-skipped) host tests that validate provisioning behavior under multiple scenarios.
- A documented `tools/` tree with at least the runtime-config hash generator plus stubs for upcoming analyzers.
- Updated onboarding docs pointing contributors to both host tests and developer tooling.

### Segment 8 – Telemetry Streamlining & Diagnostics

_Status 2025-12-31_: **Completed** – telemetry runtime config (publish interval + loop backoffs) now flows from `config/runtime/services.json` through `services_start()` into `mqtt_telemetry`, validators/build gating enforce the schema, the telemetry facade + structured logging + expanded diagnostics endpoints are live, host-side scheduler/watchdog harnesses gate regressions, and `docs/TELEMETRY_DIAGNOSTICS.md` captures the `/api/mqtt/status` payload for onboarding.

**Goal**: Finish carving `services/telemetry/` into composable layers (connection controller, publish scheduler, payload builders, diagnostics exporters) and make telemetry health observable without attaching a serial console.

- **Tasks**:
- [done] Finalize the MQTT telemetry split by moving retry/backoff constants, queue sizing, and interval knobs into a dedicated telemetry block embedded in `config/runtime/services.json`, with runtime loaders + build validation to keep behavior environment-tunable without recompiles.
- [done] Introduce a `telemetry_pipeline_t` facade that feeds both MQTT and future transports (WebSocket mirror, local buffering) from a single sensor snapshot source, eliminating direct `sensor_manager` calls from telemetry workers.
- [done] Add structured logging helpers (`telemetry_log_publish_attempt()`, `telemetry_log_drop_reason()`) to replace ad-hoc `ESP_LOGI` calls and ensure every publish attempt includes payload digest, sensor snapshot ID, and connection state.
- [done] Expand `/api/mqtt/status` (or add `/api/telemetry/status`) with queue depth, last-publish timestamp, and watchdog fault counts so dashboards can highlight stalled telemetry without reading logs.
- [done] Build host-side regression tests (`test/host/test_mqtt_scheduler.py`) that simulate bursty sensor updates, forced disconnects, and payload serialization failures to keep the telemetry stack stable during refactors.

**Deliverables**:
- `services/telemetry/README.md` describing module boundaries (payload builder, scheduler, connection controller, watchdog, diagnostics) plus how they interact with `services_start()`.
- Configurable telemetry settings (`config/runtime/telemetry.json` or an extended `services.json`) validated by `tools/validate_runtime_config.py`.
- Expanded `/api/mqtt/status` (or new endpoint) payload documented in `docs/API.md` + sample output captured in `docs/TELEMETRY_DIAGNOSTICS.md` for onboarding.
- Passing host tests that exercise telemetry scheduling and diagnostics without hardware, integrated into the existing `test/host` harness.


## Tracking + Next Steps

1. Log progress in this file after each segment completes (what landed, links to PRs/commits).
2. Keep `docs/PROJECT_STRUCTURE.md` synchronized with directory moves to avoid stale onboarding material.
3. Whenever a segment introduces new build targets, update `build.ps1` and mention them in `README.md`.
4. Consider tagging releases between major segments so QA can bisect regressions quickly.

_Last updated: 2025-12-31_
