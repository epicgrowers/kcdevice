# Connectivity Recovery Plan

## Objectives
- Ensure Wi-Fi reconnects automatically after prolonged outages without requiring a device reboot.
- Restore time synchronization and TLS-dependent services once the network returns.
- Resume MQTT telemetry and other cloud-dependent features promptly after connectivity is restored.

## Proposed Workstreams

### 1. Wi-Fi Auto-Reconnect Improvements
- Remove the fixed `MAX_RETRY_ATTEMPTS` limit in `wifi_manager.c` so disconnections keep retrying indefinitely (with backoff).
- Enable ESP-IDF's auto-reconnect (`esp_wifi_set_auto_connect(true)`) to supplement manual retries.
- Add exponential or capped backoff logging so we can observe how long the station has been offline.

### 2. Connectivity Watchdog & Recovery Hooks
- Extend the existing Wi-Fi/IP event handler to detect transitions from "offline" to "online" after a degraded period.
- When `IP_EVENT_STA_GOT_IP` fires and the system was previously degraded:
  - Reinitialize SNTP/time sync if it is not running or if it last timed out.
  - Restart MQTT telemetry (init + start) if it had been stopped/degraded.
  - Emit a "network_recovered" status event for future consumers.
- Add an "Internet watchdog" for brownouts where the STA stays associated but upstream routing fails:
  - Periodically perform a lightweight reachability test (DNS lookup, HTTPS HEAD, or MQTT ping).
  - If the check fails for N consecutive attempts, mark services degraded and trigger the same recovery path as a reconnect (time sync retry, MQTT restart).
  - Once reachability succeeds again, emit a "wan_recovered" signal so dashboards can clear the degraded badge.

### 3. Time Sync Resilience
- Expose a function (e.g., `services_request_time_sync_retry()`) that can be called after Wi-Fi recovery to restart `time_sync_init()`.
- Persist the fact that time sync completed so HTTPS/MQTT can skip blocking, but allow a re-sync if the clock drifts during long outages.

### 4. Telemetry Resume Logic
- Track MQTT client state so the watchdog can restart only what is necessary (reconnect vs. full reinit).
- Add logging when telemetry resumes after downtime to make remote monitoring easier.

## Validation Steps
1. Simulate WAN loss (disable router) and observe Wi-Fi retries until reconnection succeeds.
2. Confirm that, once WAN returns, the logs show:
   - Wi-Fi reconnect
   - SNTP/time sync restart
   - MQTT reconnect and telemetry publishes
3. Verify dashboard status indicators recover without manual reboot.
