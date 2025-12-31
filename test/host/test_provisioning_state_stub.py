#!/usr/bin/env python3
"""Host-side provisioning tests backed by a lightweight simulator."""

import unittest

from provisioning_sim import (
    ProvisioningScenario,
    ProvisioningSimulator,
    STATE_BLE_CONNECTED,
    STATE_CREDENTIALS_RECEIVED,
    STATE_PROVISIONED,
    STATE_WIFI_FAILED,
    STATE_WIFI_CONNECTING,
    STATUS_ERROR_WIFI_AUTH_FAILED,
)


class ProvisioningStateTests(unittest.TestCase):
    """Validates orchestration logic using the ProvisioningSimulator."""

    def setUp(self) -> None:
        self.simulator = ProvisioningSimulator()

    def test_connects_with_stored_credentials(self) -> None:
        scenario = ProvisioningScenario(has_stored_credentials=True)
        outcome = self.simulator.run(scenario)

        self.assertTrue(outcome.connected)
        self.assertTrue(outcome.used_stored_credentials)
        states = [t.state for t in self.simulator.transitions]
        self.assertNotIn(STATE_BLE_CONNECTED, states)
        self.assertIn(STATE_PROVISIONED, states)

    def test_ble_provisioning_happy_path(self) -> None:
        scenario = ProvisioningScenario(
            has_stored_credentials=False,
            ble_credentials={"ssid": "GrowLab", "password": "secret"},
            sensors_ready_during_provisioning=True,
        )
        outcome = self.simulator.run(scenario)

        states = [t.state for t in self.simulator.transitions]
        self.assertTrue(outcome.connected)
        self.assertTrue(outcome.provisioning_triggered)
        self.assertTrue(outcome.sensors_ready_during_provisioning)
        self.assertIn(STATE_BLE_CONNECTED, states)
        self.assertIn(STATE_CREDENTIALS_RECEIVED, states)
        self.assertIn(STATE_PROVISIONED, states)

    def test_provisioning_auth_failure(self) -> None:
        scenario = ProvisioningScenario(
            has_stored_credentials=False,
            wifi_failures_before_success=5,
            failure_status=STATUS_ERROR_WIFI_AUTH_FAILED,
        )
        outcome = self.simulator.run(scenario)

        states = [t.state for t in self.simulator.transitions]
        self.assertFalse(outcome.connected)
        self.assertEqual(outcome.failure_status, STATUS_ERROR_WIFI_AUTH_FAILED)
        self.assertIn(STATE_WIFI_FAILED, states)
        self.assertEqual(states.count(STATE_WIFI_CONNECTING), self.simulator.max_wifi_retries)

    def test_connection_guard_retries_after_drop(self) -> None:
        scenario = ProvisioningScenario(simulate_connection_drop=True)
        outcome = self.simulator.run(scenario)

        self.assertTrue(outcome.connected)
        self.assertEqual(len(self.simulator.guard_events), 2)
        self.assertEqual(self.simulator.guard_events[1]["event"], "reconnect")


if __name__ == "__main__":
    unittest.main()
