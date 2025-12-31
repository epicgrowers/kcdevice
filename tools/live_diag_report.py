#!/usr/bin/env python3
"""Continuously poll the device HTTP API for live diagnostics.

The script fetches /api/mqtt/status for MQTT health and /api/status for
broader system details so developers get a rolling snapshot without
opening a serial monitor.
"""

from __future__ import annotations

import argparse
import json
import ssl
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from typing import Any, Dict, Optional

DEFAULT_HOST = "kc-device.local"
MQTT_ENDPOINT = "/api/mqtt/status"
STATUS_ENDPOINT = "/api/status"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Live device diagnostics (MQTT + HTTP status)",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "host",
        nargs="?",
        default=DEFAULT_HOST,
        help="Device hostname or IP",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=None,
        help="HTTP(S) port exposed by the dashboard",
    )
    parser.add_argument(
        "--scheme",
        choices=("https", "http"),
        default="https",
        help="Protocol to use when connecting",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=5.0,
        help="Polling interval; set to 0 for a single snapshot",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=5.0,
        help="Request timeout per endpoint",
    )
    parser.add_argument(
        "--insecure",
        action="store_true",
        help="Skip TLS certificate verification",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Print aggregated JSON instead of a formatted summary",
    )
    parser.add_argument(
        "--no-status",
        action="store_true",
        help="Skip /api/status fetch (MQTT metrics only)",
    )
    return parser.parse_args()


def build_url(host: str, scheme: str, port: Optional[int], path: str) -> str:
    default_port = 80 if scheme == "http" else 443
    final_port = port or default_port
    return f"{scheme}://{host}:{final_port}{path}"


def fetch_json(url: str, *, timeout: float, insecure: bool) -> Dict[str, Any]:
    context = None
    if url.startswith("https://"):
        context = ssl.create_default_context()
        if insecure:
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
    request = urllib.request.Request(url, headers={"Accept": "application/json"})
    with urllib.request.urlopen(request, timeout=timeout, context=context) as response:
        body = response.read()
    return json.loads(body.decode("utf-8"))


def summarize_sensors(status: Dict[str, Any]) -> str:
    sensors = status.get("sensors")
    if isinstance(sensors, dict):
        return f"{len(sensors)} reported"
    return "unavailable"


def format_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat()


def render_summary(
    url_base: str,
    mqtt_metrics: Dict[str, Any],
    status_payload: Optional[Dict[str, Any]],
) -> None:
    timestamp = format_timestamp()
    device_id = status_payload.get("device_id") if status_payload else "unknown"
    wifi_ssid = status_payload.get("wifi_ssid") if status_payload else "?"
    ip_addr = status_payload.get("ip_address") if status_payload else "?"
    rssi = status_payload.get("rssi") if status_payload else None
    battery = status_payload.get("battery") if status_payload else None

    print(f"[{timestamp}] Live diagnostics for {device_id} via {url_base}")
    if status_payload:
        print(f"  WiFi: {wifi_ssid} ({ip_addr}), RSSI={rssi}")
        print(
            "  Sensors: ready={ready} launched={launched} cache={cache}, {summary}".format(
                ready=status_payload.get("sensors_ready"),
                launched=status_payload.get("sensor_task_launched"),
                cache=status_payload.get("sensor_interval_sec"),
                summary=summarize_sensors(status_payload),
            )
        )
        print(
            f"  Battery: {battery}%, Free heap: {status_payload.get('free_heap')} bytes, Uptime: {status_payload.get('uptime')}s"
        )

    print(
        "  MQTT: state={state} connected={connected} run={should_run} client={client_running} supervisor={supervisor_running}".format(
            state=mqtt_metrics.get("state"),
            connected=mqtt_metrics.get("connected"),
            should_run=mqtt_metrics.get("should_run"),
            client_running=mqtt_metrics.get("client_running"),
            supervisor_running=mqtt_metrics.get("supervisor_running"),
        )
    )
    print(
        "         reconnects={reconnect} consecutive_failures={failures} last_transition={last} ({delta}s ago)".format(
            reconnect=mqtt_metrics.get("reconnect_count"),
            failures=mqtt_metrics.get("consecutive_failures"),
            last=mqtt_metrics.get("last_transition_us"),
            delta=mqtt_metrics.get("seconds_since_transition"),
        )
    )
    print()


def main() -> int:
    args = parse_args()
    mqtt_url = build_url(args.host, args.scheme, args.port, MQTT_ENDPOINT)
    status_url = (
        None
        if args.no_status
        else build_url(args.host, args.scheme, args.port, STATUS_ENDPOINT)
    )

    interval = args.interval if args.interval and args.interval > 0 else None

    while True:
        try:
            mqtt_metrics = fetch_json(mqtt_url, timeout=args.timeout, insecure=args.insecure)
            status_payload = (
                fetch_json(status_url, timeout=args.timeout, insecure=args.insecure)
                if status_url
                else None
            )
        except urllib.error.HTTPError as exc:
            print(f"HTTP error {exc.code}: {exc.reason}", file=sys.stderr)
            if interval is None:
                return 2
        except urllib.error.URLError as exc:
            print(f"Connection error: {exc.reason}", file=sys.stderr)
            if interval is None:
                return 2
        except (ssl.SSLError, TimeoutError) as exc:
            print(f"TLS/timeout error: {exc}", file=sys.stderr)
            if interval is None:
                return 2
        else:
            if args.json:
                aggregated = {
                    "timestamp": format_timestamp(),
                    "mqtt": mqtt_metrics,
                    "status": status_payload,
                }
                print(json.dumps(aggregated, indent=2, sort_keys=True))
            else:
                base_label = f"{args.scheme}://{args.host}:{args.port or (80 if args.scheme == 'http' else 443)}"
                render_summary(base_label, mqtt_metrics, status_payload)

            if interval is None:
                return 0

        if interval is not None:
            try:
                time.sleep(interval)
            except KeyboardInterrupt:
                print("Interrupted, exiting")
                return 0


if __name__ == "__main__":
    sys.exit(main())
