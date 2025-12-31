# Telemetry Diagnostics

The `/api/mqtt/status` endpoint exposes the MQTT connection state machine along with telemetry pipeline metrics so you can validate broker health and publish cadence without attaching a serial cable. The `tools/mqtt_status_probe.py` helper fetches and formats this payload from any developer machine.

## Quick start

1. Connect to the same network as the device (mDNS hostname `kc.local` by default).
2. Run the probe with certificate validation disabled (the dashboard uses a self-signed cert in development):
   ```powershell
   C:/Code/kc_device/.venv/Scripts/python.exe tools/mqtt_status_probe.py kc.local --insecure --json
   ```
3. Use the JSON view to copy diagnostics into bug reports or checklists. Omit `--json` for a human-friendly summary.

## Sample payload

```
{
  "client_running": true,
  "connected": true,
  "consecutive_failures": 0,
  "last_transition_us": 16517205,
  "reconnect_count": 0,
  "seconds_since_transition": 699.440897,
  "should_run": true,
  "state": "connected",
  "state_code": 2,
  "supervisor_running": true,
  "telemetry": {
    "busy_backoff_ms": 1000,
    "client_backoff_ms": 750,
    "idle_delay_ms": 200,
    "interval_enabled": false,
    "last_battery_percent": 94.01953125,
    "last_battery_valid": true,
    "last_capture_us": 713591740,
    "last_publish_us": 713643795,
    "last_rssi": -61,
    "last_sensor_count": 4,
    "last_sequence_id": 215,
    "manual_request_pending": false,
    "publish_interval_sec": 0,
    "publish_queue_depth": 0,
    "scheduler_last_publish_us": 713646804,
    "scheduler_next_retry_us": -1,
    "seconds_since_capture": 2.366362,
    "seconds_since_publish": 2.314307,
    "seconds_since_scheduler_publish": 2.311298,
    "seconds_until_next_retry": -1,
    "total_attempts": 214,
    "total_failure": 0,
    "total_success": 214
  },
  "watchdog": {
    "active": true,
    "poll_interval_ms": 5000,
    "report_interval_ms": 30000,
    "seconds_since_last_report": -1,
    "stuck_threshold_ms": 60000,
    "trip_count": 0,
    "tripped": false,
    "unhealthy_duration_ms": -1
  }
}
```

## Field guide

- **Top-level MQTT state**
  - `state`/`state_code`: Mirrors the firmware enum so you can map directly to `mqtt_state_t`.
  - `consecutive_failures` and `reconnect_count`: Track reconnect churn; spikes indicate broker or Wi-Fi instability.
  - `seconds_since_transition`: Useful for spotting stuck states when the numbers grow without bound.
- **Telemetry object**
  - `total_attempts`, `total_success`, `total_failure`: Count pipeline publish attempts for regression tracking.
  - `last_sequence_id`, `last_sensor_count`, `last_rssi`: Snapshot of the most recent telemetry bundle.
  - `scheduler_*` and `seconds_until_next_retry`: Reflect the publish scheduler’s timers, making it clear when manual/interval requests are blocked.
  - `busy_backoff_ms`, `client_backoff_ms`: Tunables that gate retries after payload assembly errors or forced disconnects.
- **Watchdog object**
  - `trip_count`/`tripped`: Flags sustained disconnects caught by `mqtt_connection_watchdog`.
  - `unhealthy_duration_ms`: How long the watchdog has considered the client unhealthy.
  - `report_interval_ms` + `seconds_since_last_report`: Aid in verifying the watchdog’s rate limiting.

## Related tooling

- Host regression tests in `test/host/test_mqtt_scheduler.py` exercise the telemetry scheduler math via `mqtt_scheduler_sim.py` so edge cases can be covered off-target.
- `test/host/test_mqtt_watchdog_sim.py` continues to validate watchdog heuristics alongside the diagnostics payload shown above.
