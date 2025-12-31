from __future__ import annotations

import unittest

from mqtt_watchdog_sim import (
    MQTT_STATE_CONNECTED,
    MQTT_STATE_CONNECTING,
    MQTT_STATE_DISCONNECTED,
    MqttMetrics,
    MqttWatchdogHarness,
    render_mqtt_http_status,
)


class MqttWatchdogHarnessTest(unittest.TestCase):
    def test_disconnect_storm_triggers_watchdog(self) -> None:
        harness = MqttWatchdogHarness(poll_interval_ms=100, stuck_threshold_ms=250, report_interval_ms=200)
        metrics = MqttMetrics(state=MQTT_STATE_CONNECTING, should_run=True, supervisor_running=True)

        harness.tick(metrics, 100)
        self.assertEqual(len(harness.events), 0)

        harness.tick(metrics, 100)
        self.assertEqual(len(harness.events), 0)

        event = harness.tick(metrics, 100)
        self.assertIsNotNone(event)
        self.assertEqual(len(harness.events), 1)
        self.assertEqual(event["state"], MQTT_STATE_CONNECTING)
        self.assertGreaterEqual(event["elapsed_ms"], 250)

    def test_watchdog_rate_limits_reports(self) -> None:
        harness = MqttWatchdogHarness(poll_interval_ms=100, stuck_threshold_ms=100, report_interval_ms=300)
        metrics = MqttMetrics(state=MQTT_STATE_DISCONNECTED, should_run=True, supervisor_running=True)

        for _ in range(5):
            harness.tick(metrics, 100)
        self.assertEqual(len(harness.events), 1)

        # Remains unhealthy but reports again only after report_interval_ms
        harness.tick(metrics, 100)
        self.assertEqual(len(harness.events), 1)
        harness.tick(metrics, 200)
        self.assertEqual(len(harness.events), 2)

    def test_http_status_payload_matches_endpoint(self) -> None:
        metrics = MqttMetrics(
            state=MQTT_STATE_CONNECTED,
            should_run=True,
            client_running=True,
            supervisor_running=True,
            reconnect_count=3,
            consecutive_failures=0,
            last_transition_us=1_000_000,
        )
        payload = render_mqtt_http_status(metrics, now_us=2_500_000)
        self.assertTrue(payload["connected"])
        self.assertEqual(payload["state_code"], 2)
        self.assertAlmostEqual(payload["seconds_since_transition"], 1.5)


if __name__ == "__main__":
    unittest.main()
