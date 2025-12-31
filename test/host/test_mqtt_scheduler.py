import unittest

from mqtt_scheduler_sim import TelemetryPublishHarness


class TelemetrySchedulerTests(unittest.TestCase):
    def test_bursty_sensor_updates_respect_interval(self) -> None:
        harness = TelemetryPublishHarness(interval_sec=10)
        harness.queue_samples(2)

        harness.scheduler.request_publish()
        result_first = harness.attempt_publish()
        self.assertEqual(result_first["action"], "published")
        self.assertEqual(result_first["sequence_id"], 1)

        result_wait = harness.attempt_publish()
        self.assertEqual(result_wait["action"], "wait")
        self.assertGreaterEqual(result_wait["wait_ms"], 10_000)

        result_second = harness.attempt_publish()
        self.assertEqual(result_second["action"], "published")
        self.assertEqual(result_second["sequence_id"], 2)
        self.assertEqual(harness.pipeline.metrics["total_success"], 2)

    def test_forced_disconnect_records_failure_and_backoff(self) -> None:
        harness = TelemetryPublishHarness(interval_sec=5, client_backoff_ms=500)
        harness.queue_samples(1)

        harness.scheduler.request_publish()
        result = harness.attempt_publish(client_available=False)
        self.assertEqual(result["action"], "client-unavailable")
        self.assertEqual(harness.pipeline.metrics["total_failure"], 1)
        self.assertFalse(harness.pipeline.metrics["last_publish_success"])
        self.assertGreaterEqual(harness.scheduler.next_retry_us, harness.now_us)

        wait_result = harness.attempt_publish()
        self.assertEqual(wait_result["action"], "wait")
        self.assertGreater(wait_result["wait_ms"], 0)

    def test_payload_failures_update_pipeline_metrics(self) -> None:
        harness = TelemetryPublishHarness(interval_sec=2, busy_backoff_ms=400)
        harness.queue_samples(2)

        harness.scheduler.request_publish()
        failure_result = harness.attempt_publish(payload_ok=False)
        self.assertEqual(failure_result["action"], "payload-error")
        self.assertEqual(harness.pipeline.metrics["total_attempts"], 1)
        self.assertEqual(harness.pipeline.metrics["total_failure"], 1)

        harness.scheduler.request_publish()
        success_result = harness.attempt_publish()
        self.assertEqual(success_result["action"], "published")
        self.assertEqual(harness.pipeline.metrics["total_success"], 1)


if __name__ == "__main__":
    unittest.main()
