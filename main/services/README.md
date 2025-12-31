# Cloud Services Module

This directory houses every network-facing or cloud integration module. `app_main()` and the boot
coordinator talk only to `services/core`, while each service keeps its own focused subdirectory.

## Layout

- `core/`: lifecycle APIs (`services_start()`, `services_stop()`), `services_config_t` defaults, and
  dependency injection helpers (`services_dependencies_t`).
- `http/`: HTTPS dashboard, REST handlers, WebSocket streaming, and dashboard asset helpers.
- `telemetry/`: MQTT clients plus any future background telemetry or analytics tasks.
- `discovery/`: mDNS and other LAN discovery helpers.
- `time_sync/`: SNTP/RTC synchronization utilities.
- `keys/`: API key storage plus related credential helpers.
- `provisioning/`: Cloud provisioning helpers that fetch TLS material and persist device metadata.

Each submodule should expose a header so other components only include the parts they need. Avoid
touching global state directly—propagate readiness or fault information back through the services
core so the boot coordinator stays authoritative.

## Core APIs
1. `services_config_load_defaults()` seeds a `services_config_t` with conservative defaults. The
	network task updates TLS flags, MQTT credentials, publish intervals, and optional callbacks
	before calling `services_start()`.
2. `services_provisioning_api_t` centralizes TLS assets and device metadata fetchers. `network_boot`
	builds one provider (wrapping `cloud_provisioning` today) and stores it on
	`services_config_t`, letting HTTP/MQTT consume certificates, IDs, and device names without
	including provisioning headers directly.
3. `services_dependencies_t` carries shared handles (`boot_event_group`, ready/degraded bits,
	optional sensor pipeline context, and the new `services_fault_handler_t`). This keeps FreeRTOS
	primitives and boot-monitor hooks out of feature modules.
4. `services_start()` launches each enabled service sequentially, emits `SERVICES_EVENT_*`
	callbacks, applies per-service readiness/degradation bookkeeping, and finally sets the boot
	event bits. `services_stop()` unwinds the stack in reverse order and clears the degraded bit so
	a future restart can report a clean slate.

## Event Bits & Degradation
- `NETWORK_READY_BIT` is asserted after at least one service is running (or every optional service
  was intentionally disabled). This mirrors the previous behavior so `app_main()` can unblock as
  soon as the device has some level of connectivity.
- `NETWORK_DEGRADED_BIT` latches the first time any component emits `SERVICES_EVENT_DEGRADED`.
  `app_main()` now waits on `NETWORK_READY_BIT | NETWORK_DEGRADED_BIT`, so the boot flow either
  continues with a healthy services stack or exits early knowing the device must run in a
  best-effort mode.
- `services_fault_handler_t` gives callers an immediate callback whenever degradation occurs. The
  default handler (wired up in `network_task`) publishes a concise `network_fault` stage via
  `boot_monitor_publish()`, but other builds can swap in watchdog or reboot logic without touching
  the services implementation.

## Adding a New Service
1. Place sources under a new `services/<name>/` directory and expose a minimal header consumed by
	`services/core/services.c`.
2. Extend `services_config_t` if the service needs runtime knobs (ports, credentials, feature
	toggles). Seed sane defaults inside `services_config_load_defaults()` so call sites never have
	to zero structs manually.
3. Update `services_start()` to launch the module, emit STARTING/READY/DEGRADED/STOPPED events, and
	make sure failures call `services_emit_event(..., SERVICES_EVENT_DEGRADED, ...)` so the core can
	set the degraded bit.
4. Refresh `docs/PROJECT_STRUCTURE.md` and re-run `build.ps1` to verify the component compiles into
	all supported targets (ESP32-S3/C6 variants share the same tree).
