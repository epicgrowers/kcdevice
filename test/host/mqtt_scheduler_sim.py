"""Telemetry publish scheduler + pipeline harness for host-side tests.

This module mirrors the key behaviors of the firmware's telemetry pipeline,
MQTT publish scheduler, and the surrounding FreeRTOS task so we can validate
interval handling, manual publish requests, and failure bookkeeping without
flashing hardware.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import List, Optional

DEFAULT_IDLE_MS = 200
OFFLINE_BACKOFF_MS = 1000


@dataclass
class TelemetrySample:
    """Represents a queued sensor snapshot."""

    sensor_count: int = 4
    battery_valid: bool = True
    battery_percent: float = 95.0
    rssi: int = -55


class TelemetryPipelineSim:
    """Lightweight recreation of telemetry_pipeline.c metrics handling."""

    def __init__(self) -> None:
        self._sequence_id = 0
        self._samples: List[TelemetrySample] = []
        self.metrics = {
            "last_sequence_id": 0,
            "last_capture_us": 0,
            "last_publish_us": 0,
            "total_attempts": 0,
            "total_success": 0,
            "total_failure": 0,
            "last_publish_success": False,
            "last_sensor_count": 0,
            "last_rssi": 0,
            "last_battery_valid": False,
            "last_battery_percent": 0.0,
        }

    def queue_sample(self, sample: TelemetrySample) -> None:
        self._samples.append(sample)

    def acquire_sample(self, now_us: int) -> Optional[dict]:
        if not self._samples:
            return None
        sample = self._samples.pop(0)
        self._sequence_id += 1
        capture = {
            "sequence_id": self._sequence_id,
            "captured_at_us": now_us,
            "sample": sample,
        }
        self.metrics["last_sequence_id"] = self._sequence_id
        self.metrics["last_capture_us"] = now_us
        self.metrics["last_sensor_count"] = sample.sensor_count
        self.metrics["last_rssi"] = sample.rssi
        self.metrics["last_battery_valid"] = sample.battery_valid
        self.metrics["last_battery_percent"] = sample.battery_percent
        return capture

    def record_publish_result(self, sample_capture: Optional[dict], success: bool, now_us: int) -> None:
        self.metrics["total_attempts"] += 1
        if success:
            self.metrics["total_success"] += 1
        else:
            self.metrics["total_failure"] += 1
        self.metrics["last_publish_success"] = success
        self.metrics["last_publish_us"] = now_us

        if sample_capture is None:
            return

        sample = sample_capture["sample"]
        self.metrics["last_sequence_id"] = sample_capture["sequence_id"]
        self.metrics["last_capture_us"] = sample_capture["captured_at_us"]
        self.metrics["last_sensor_count"] = sample.sensor_count
        self.metrics["last_rssi"] = sample.rssi
        self.metrics["last_battery_valid"] = sample.battery_valid
        self.metrics["last_battery_percent"] = sample.battery_percent


@dataclass
class SchedulerDecision:
    should_publish: bool = False
    should_block: bool = False
    wait_ms: int = DEFAULT_IDLE_MS


class MqttPublishSchedulerSim:
    """Python port of mqtt_publish_scheduler.c logic."""

    def __init__(self, interval_sec: int) -> None:
        self.interval_sec = interval_sec
        self.last_publish_us = 0
        self.next_retry_us = 0
        self.manual_request = False

    def set_interval(self, interval_sec: int) -> None:
        self.interval_sec = interval_sec

    def request_publish(self) -> None:
        self.manual_request = True

    def note_publish(self, now_us: int) -> None:
        self.last_publish_us = now_us
        self.next_retry_us = 0
        self.manual_request = False

    def delay(self, delay_ms: int, now_us: int) -> None:
        candidate = now_us + delay_ms * 1000
        if candidate > self.next_retry_us:
            self.next_retry_us = candidate

    def next_action(self, now_us: int, mqtt_connected: bool) -> SchedulerDecision:
        decision = SchedulerDecision()

        if not mqtt_connected:
            self.delay(OFFLINE_BACKOFF_MS, now_us)
            decision.wait_ms = self._compute_wait(now_us, now_us + OFFLINE_BACKOFF_MS * 1000)
            return decision

        interval_enabled = self.interval_sec > 0
        interval_us = self.interval_sec * 1_000_000 if interval_enabled else 0

        if not interval_enabled and not self.manual_request:
            decision.should_block = True
            decision.wait_ms = 0
            return decision

        due = self.manual_request
        next_due_us = now_us
        if not due and interval_enabled:
            if self.last_publish_us == 0:
                due = True
            else:
                next_due_us = self.last_publish_us + interval_us
                if now_us >= next_due_us:
                    due = True

        if not due:
            decision.wait_ms = self._compute_wait(now_us, next_due_us)
            return decision

        if self.next_retry_us > now_us and not self.manual_request:
            decision.wait_ms = self._us_to_ms(self.next_retry_us - now_us)
            return decision

        self.manual_request = False
        decision.should_publish = True
        decision.wait_ms = 0
        return decision

    def _compute_wait(self, now_us: int, target_us: int) -> int:
        effective_target = target_us
        if self.next_retry_us > now_us and self.next_retry_us > effective_target:
            effective_target = self.next_retry_us
        if effective_target <= now_us:
            return DEFAULT_IDLE_MS
        return self._us_to_ms(effective_target - now_us)

    @staticmethod
    def _us_to_ms(value_us: int) -> int:
        return max(int(value_us / 1000), DEFAULT_IDLE_MS)


class TelemetryPublishHarness:
    """Coordinator that wires the scheduler and pipeline together."""

    def __init__(
        self,
        *,
        interval_sec: int = 10,
        busy_backoff_ms: int = 1000,
        client_backoff_ms: int = 750,
    ) -> None:
        self.scheduler = MqttPublishSchedulerSim(interval_sec)
        self.pipeline = TelemetryPipelineSim()
        self.busy_backoff_ms = busy_backoff_ms
        self.client_backoff_ms = client_backoff_ms
        self.now_us = 0

    def advance(self, delta_ms: int) -> None:
        self.now_us += delta_ms * 1000

    def queue_samples(self, count: int, sample: Optional[TelemetrySample] = None) -> None:
        sample = sample or TelemetrySample()
        for _ in range(count):
            self.pipeline.queue_sample(sample)

    def attempt_publish(self, *, client_available: bool = True, payload_ok: bool = True) -> dict:
        if self.now_us == 0:
            self.now_us = 1
        decision = self.scheduler.next_action(self.now_us, mqtt_connected=True)
        if not decision.should_publish:
            self.advance(decision.wait_ms)
            return {"action": "wait", "wait_ms": decision.wait_ms}

        sample_capture = self.pipeline.acquire_sample(self.now_us)
        if sample_capture is None:
            self.scheduler.delay(self.busy_backoff_ms, self.now_us)
            return {"action": "snapshot-missing"}
        if not client_available:
            self.pipeline.record_publish_result(sample_capture, success=False, now_us=self.now_us)
            self.scheduler.delay(self.client_backoff_ms, self.now_us)
            return {"action": "client-unavailable"}

        if not payload_ok:
            self.pipeline.record_publish_result(sample_capture, success=False, now_us=self.now_us)
            self.scheduler.delay(self.busy_backoff_ms, self.now_us)
            return {"action": "payload-error"}

        self.pipeline.record_publish_result(sample_capture, success=True, now_us=self.now_us)
        self.scheduler.note_publish(self.now_us)
        return {"action": "published", "sequence_id": sample_capture["sequence_id"]}
