"""Provisioning simulator used by host-side unit tests.

The simulator mirrors the high-level state machine transitions without relying on
ESP-IDF symbols so the Python tests can validate orchestration logic off-target.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional, Tuple

# State constants mirror provisioning_state.h for easier mental mapping.
STATE_IDLE = "PROV_STATE_IDLE"
STATE_BLE_CONNECTED = "PROV_STATE_BLE_CONNECTED"
STATE_CREDENTIALS_RECEIVED = "PROV_STATE_CREDENTIALS_RECEIVED"
STATE_WIFI_CONNECTING = "PROV_STATE_WIFI_CONNECTING"
STATE_PROVISIONED = "PROV_STATE_PROVISIONED"
STATE_WIFI_FAILED = "PROV_STATE_WIFI_FAILED"

# Status constants mirror provisioning_status_t.
STATUS_SUCCESS = "STATUS_SUCCESS"
STATUS_ERROR_WIFI_AUTH_FAILED = "STATUS_ERROR_WIFI_AUTH_FAILED"
STATUS_ERROR_WIFI_TIMEOUT = "STATUS_ERROR_WIFI_TIMEOUT"


@dataclass
class Transition:
    state: str
    status: str
    message: str


@dataclass
class ProvisioningScenario:
    has_stored_credentials: bool = True
    ble_credentials: Optional[dict] = None
    wifi_failures_before_success: int = 0
    failure_status: Optional[str] = None
    sensors_ready_during_provisioning: bool = False
    simulate_connection_drop: bool = False


@dataclass
class ProvisioningOutcome:
    connected: bool
    used_stored_credentials: bool
    provisioning_triggered: bool
    sensors_ready_during_provisioning: bool
    retries: int
    failure_status: Optional[str] = None


class ProvisioningSimulator:
    """Lightweight orchestrator that mimics provisioning_run() decisions."""

    def __init__(self, max_wifi_retries: int = 5, guard_interval_sec: int = 30) -> None:
        self.max_wifi_retries = max_wifi_retries
        self.guard_interval_sec = guard_interval_sec
        self.transitions: List[Transition] = []
        self.guard_events: List[dict] = []

    def run(self, scenario: ProvisioningScenario) -> ProvisioningOutcome:
        """Execute the scenario and return the simulated outcome."""

        self.transitions.clear()
        self.guard_events.clear()
        self._emit(STATE_IDLE, STATUS_SUCCESS, "Provisioning runner started")

        if scenario.has_stored_credentials:
            used_stored_credentials = True
            provisioning_triggered = False
            connected, retries, failure_status = self._simulate_wifi_connect(
                context="Stored credentials",
                failures_before_success=scenario.wifi_failures_before_success,
                failure_status=scenario.failure_status,
            )
        else:
            used_stored_credentials = False
            provisioning_triggered = True
            connected, retries, failure_status = self._simulate_ble_flow(scenario)

        if connected and scenario.simulate_connection_drop:
            self._simulate_connection_guard_event()

        return ProvisioningOutcome(
            connected=connected,
            used_stored_credentials=used_stored_credentials,
            provisioning_triggered=provisioning_triggered,
            sensors_ready_during_provisioning=scenario.sensors_ready_during_provisioning,
            retries=retries,
            failure_status=failure_status,
        )

    def _simulate_ble_flow(self, scenario: ProvisioningScenario) -> Tuple[bool, int, Optional[str]]:
        self._emit(STATE_BLE_CONNECTED, STATUS_SUCCESS, "BLE client connected")
        ssid = (scenario.ble_credentials or {}).get("ssid", "ble-ssid")
        self._emit(
            STATE_CREDENTIALS_RECEIVED,
            STATUS_SUCCESS,
            f"Credentials received for {ssid}",
        )
        return self._simulate_wifi_connect(
            context="BLE provisioning",
            failures_before_success=scenario.wifi_failures_before_success,
            failure_status=scenario.failure_status,
        )

    def _simulate_wifi_connect(
        self,
        *,
        context: str,
        failures_before_success: int,
        failure_status: Optional[str],
    ) -> Tuple[bool, int, Optional[str]]:
        failures = min(failures_before_success, self.max_wifi_retries)
        for attempt in range(1, failures + 1):
            self._emit(
                STATE_WIFI_CONNECTING,
                STATUS_SUCCESS,
                f"{context}: Connecting... (attempt {attempt}/{self.max_wifi_retries})",
            )

        if failures_before_success >= self.max_wifi_retries and failure_status:
            self._emit(
                STATE_WIFI_FAILED,
                failure_status,
                f"{context}: WiFi failed after {self.max_wifi_retries} attempts",
            )
            return False, self.max_wifi_retries, failure_status

        success_attempt = failures + 1
        self._emit(
            STATE_WIFI_CONNECTING,
            STATUS_SUCCESS,
            f"{context}: Connecting... (attempt {success_attempt}/{self.max_wifi_retries})",
        )
        self._emit(STATE_PROVISIONED, STATUS_SUCCESS, "WiFi connected")
        return True, failures, None

    def _simulate_connection_guard_event(self) -> None:
        self.guard_events.append(
            {
                "event": "disconnect",
                "message": "WiFi drop detected",
            }
        )
        self.guard_events.append(
            {
                "event": "reconnect",
                "message": f"Reconnecting after {self.guard_interval_sec}s",
            }
        )

    def _emit(self, state: str, status: str, message: str) -> None:
        self.transitions.append(Transition(state=state, status=status, message=message))
