# Services Namespace

This directory houses every network-facing or cloud integration module. The goal is to let `app_main()` and the boot coordinator talk to a single orchestration surface (`services/core`) while each service keeps its own focused subdirectory.

## Layout

- `core/`: shared configs, lifecycle APIs (`services_start()`, `services_stop()`), readiness tracking, and dependency injection helpers.
- `http/`: HTTPS dashboard, REST handlers, WebSocket streaming, and dashboard asset helpers.
- `telemetry/`: MQTT clients plus any future background telemetry or analytics tasks.
- `discovery/`: mDNS and other LAN discovery helpers.
- `time_sync/`: SNTP/RTC synchronization utilities.
- `keys/`: API key storage plus related credential helpers.

Each submodule should provide its own header so other components only include the part they need. Whenever a service has side effects that the boot layer cares about (e.g., readiness, degradation), report those events back to the core via `services_status_listener_t` instead of poking global state.
