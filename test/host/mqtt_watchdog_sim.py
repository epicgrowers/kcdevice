"""MQTT watchdog + status harness for host-side testing.

This module mirrors the logic baked into mqtt_connection_watchdog.c and the
/api/mqtt/status handler so unit tests can simulate disconnect storms without
flashing firmware.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import List, Optional

MQTT_STATE_DISCONNECTED = "MQTT_STATE_DISCONNECTED"
MQTT_STATE_CONNECTING = "MQTT_STATE_CONNECTING"
MQTT_STATE_CONNECTED = "MQTT_STATE_CONNECTED"
MQTT_STATE_ERROR = "MQTT_STATE_ERROR"


@dataclass
class MqttMetrics:
    state: str = MQTT_STATE_DISCONNECTED
    should_run: bool = True
    client_running: bool = False
    supervisor_running: bool = True
    reconnect_count: int = 0
    consecutive_failures: int = 0
    last_transition_us: int = 0


class MqttWatchdogHarness:
    """Pure-Python recreation of the MQTT watchdog timing logic."""

    def __init__(
        self,
        *,
        poll_interval_ms: int = 5000,
        stuck_threshold_ms: int = 60000,
        report_interval_ms: int = 30000,
    ) -> None:
        self.poll_interval_ms = poll_interval_ms
        self.stuck_threshold_ms = stuck_threshold_ms
        self.report_interval_ms = report_interval_ms
        self._now_ms = 0
        self._unhealthy_since_ms: Optional[int] = None
        self._last_report_ms: Optional[int] = None
        self._tripped = False
        self.events: List[dict] = []

    def reset(self) -> None:
        self._unhealthy_since_ms = None
        self._last_report_ms = None
        self._tripped = False
        self.events.clear()
        self._now_ms = 0

    def tick(self, metrics: MqttMetrics, step_ms: Optional[int] = None) -> Optional[dict]:
        """Advance the simulated timer and feed the latest metrics snapshot."""

        step = self.poll_interval_ms if step_ms is None else step_ms
        self._now_ms += step

        if not self._should_monitor(metrics):
            self._reset_watchdog_state()
            return None

        if metrics.state == MQTT_STATE_CONNECTED:
            self._reset_watchdog_state()
            return None

        if self._unhealthy_since_ms is None:
            initial_reference = self._now_ms - step
            self._unhealthy_since_ms = initial_reference if initial_reference >= 0 else 0
            return None

        elapsed_ms = self._now_ms - self._unhealthy_since_ms
        if elapsed_ms < self.stuck_threshold_ms:
            return None

        if not self._tripped:
            return self._record_event(metrics.state, elapsed_ms)

        min_gap_ms = self.report_interval_ms + self.poll_interval_ms
        since_last_report = (
            self._now_ms - self._last_report_ms
            if self._last_report_ms is not None
            else min_gap_ms
        )
        if since_last_report > min_gap_ms:
            return self._record_event(metrics.state, elapsed_ms)

        return None

    def _should_monitor(self, metrics: MqttMetrics) -> bool:
        if not metrics.should_run:
            return False
        if not metrics.supervisor_running:
            return True
        return metrics.state != MQTT_STATE_CONNECTED

    def _reset_watchdog_state(self) -> None:
        self._unhealthy_since_ms = None
        self._tripped = False
        self._last_report_ms = None

    def _record_event(self, state: str, elapsed_ms: int) -> dict:
        event = {
            "timestamp_ms": self._now_ms,
            "state": state,
            "elapsed_ms": elapsed_ms,
        }
        self.events.append(event)
        self._tripped = True
        self._last_report_ms = self._now_ms
        return event


def render_mqtt_http_status(metrics: MqttMetrics, now_us: Optional[int] = None) -> dict:
    """Return a dict that mirrors /api/mqtt/status payloads."""

    now_us = now_us if now_us is not None else metrics.last_transition_us
    payload = {
        "state": metrics.state,
        "state_code": _state_to_code(metrics.state),
        "should_run": metrics.should_run,
        "client_running": metrics.client_running,
        "supervisor_running": metrics.supervisor_running,
        "connected": metrics.state == MQTT_STATE_CONNECTED,
        "reconnect_count": metrics.reconnect_count,
        "consecutive_failures": metrics.consecutive_failures,
        "last_transition_us": float(metrics.last_transition_us),
    }

    if metrics.last_transition_us > 0 and now_us is not None and now_us >= metrics.last_transition_us:
        delta_sec = (now_us - metrics.last_transition_us) / 1_000_000.0
    else:
        delta_sec = -1
    payload["seconds_since_transition"] = delta_sec
    return payload


def _state_to_code(state: str) -> int:
    mapping = {
        MQTT_STATE_DISCONNECTED: 0,
        MQTT_STATE_CONNECTING: 1,
        MQTT_STATE_CONNECTED: 2,
        MQTT_STATE_ERROR: 3,
    }
    return mapping.get(state, -1)
