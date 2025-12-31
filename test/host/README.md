# Host-Side Provisioning Tests

This directory collects Python-based harnesses that exercise the provisioning state machine and helper surfaces without flashing firmware. The tests rely on `provisioning_sim.py`, a lightweight simulator that mirrors the orchestration performed by `provisioning_run()` so we can validate control-flow decisions off-target.

## Running the suite

```powershell
python -m unittest discover -s test/host -p "test_*.py"
```

## Current coverage roadmap

1. Stored-credential reuse should never trigger BLE provisioning (implemented).
2. BLE provisioning happy path pushes credentials through the same Wi-Fi connect path and lands in `PROV_STATE_PROVISIONED` (implemented).
3. Wi-Fi failures surface the right status codes (`STATUS_ERROR_WIFI_AUTH_FAILED`, etc.) and stop after the configured retries (implemented).
4. The connection guard recovers links after configured backoff intervals (simulated today; replace with real guard hooks once exposed from firmware).

Add new tests or extend the simulator as we expose more host-callable hooks (e.g., MQTT telemetry injection, Wi-Fi error mapping).

## MQTT watchdog + status harness

`mqtt_watchdog_sim.py` mirrors the firmware's MQTT connection watchdog and the `/api/mqtt/status` response payload so we can stress-test disconnect storms on a laptop. The accompanying `test_mqtt_watchdog_sim.py` suite verifies that:

1. Extended CONNECTING/DISCONNECTED runs eventually trip the watchdog and emit degradation events.
2. Reports are rate-limited to match the firmware's `report_interval_ms` guard.
3. The synthetic HTTP payload matches what the on-device handler returns (state codes, timers, and counters).

Use this harness when validating new watchdog heuristics or when tweaking the HTTP diagnostic surface without reflashing hardware.

## Telemetry scheduler harness

`mqtt_scheduler_sim.py` recreates the telemetry pipeline metrics and MQTT publish scheduler so bursty sample captures, forced disconnects, and payload failures can be tested entirely on the host. The `test_mqtt_scheduler.py` cases assert that:

1. Manual/interval publishes respect the configured cadence even when sensors fire back-to-back.
2. Forced disconnects increment failure counters and apply the expected retry backoff.
3. Payload assembly failures update telemetry diagnostics before the next publish attempt.

Reach for this harness whenever the telemetry diagnostics surface or scheduler math changes—you can validate the edge cases without touching hardware logs.
