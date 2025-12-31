#!/usr/bin/env python3
"""Host-side unit test stubs for provisioning state transitions.

These tests are intentionally skipped until the provisioning package exposes
mockable hooks (Segment 2 deliverable). Each stub documents the scenario that
will eventually be validated so contributors know where to add real asserts.
"""

import unittest


class ProvisioningStateTests(unittest.TestCase):
    """Placeholder scenarios for the provisioning state machine."""

    @unittest.skip("Needs provisioning_runner hooks to simulate stored credentials")
    def test_connects_with_stored_credentials(self):
        """Should stay in PROVISIONED without invoking BLE provisioning when creds exist."""

    @unittest.skip("Needs BLE provisioning harness to inject credential payloads")
    def test_ble_provisioning_happy_path(self):
        """Should walk BLE_CONNECTED -> CREDENTIALS_RECEIVED -> PROVISIONED with success logs."""

    @unittest.skip("Needs wifi_manager error surface to assert STATUS_ERROR_WIFI_AUTH_FAILED")
    def test_provisioning_auth_failure(self):
        """Should emit WIFI_FAILED state and bubble auth failure back to callers."""

    @unittest.skip("Needs connection guard plumbing so reconnect attempts can be observed")
    def test_connection_guard_retries_after_drop(self):
        """Should trigger wifi_manager_connect at the configured retry interval when offline."""


if __name__ == "__main__":
    unittest.main()
