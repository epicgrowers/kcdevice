#!/usr/bin/env python3
"""Fetch MQTT connection metrics from the device dashboard.

This helper hits the /api/mqtt/status endpoint exposed by the HTTPS
server so developers can confirm broker connectivity without opening the
ESP-IDF monitor or digging through logs.
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
from typing import Any, Dict

DEFAULT_HOST = "kc-device.local"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Query /api/mqtt/status for quick MQTT diagnostics",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "host",
        nargs="?",
        default=DEFAULT_HOST,
        help="Device hostname or IP (defaults to kc-device.local)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=None,
        help="HTTP(S) port exposed by the dashboard"
    )
    parser.add_argument(
        "--scheme",
        choices=("https", "http"),
        default="https",
        help="Protocol to use when connecting to the dashboard",
    )
    parser.add_argument(
        "--insecure",
        action="store_true",
        help="Skip TLS certificate verification (useful with self-signed certs)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=5.0,
        help="Request timeout in seconds",
    )
    parser.add_argument(
        "--watch",
        type=float,
        metavar="SECONDS",
        help="Poll continuously every N seconds instead of exiting after the first fetch",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Print the raw JSON payload instead of the formatted summary",
    )
    return parser.parse_args()


def build_url(host: str, scheme: str, port: int | None) -> str:
    default_port = 80 if scheme == "http" else 443
    final_port = port or default_port
    return f"{scheme}://{host}:{final_port}/api/mqtt/status"


def fetch_metrics(url: str, *, timeout: float, insecure: bool) -> Dict[str, Any]:
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


def format_timestamp(us_value: float | int | None) -> str:
    if not us_value or us_value <= 0:
        return "unknown"
    timestamp = datetime.fromtimestamp(us_value / 1_000_000.0, tz=timezone.utc)
    return timestamp.isoformat()


def format_delta(seconds: float | int | None) -> str:
    if seconds is None or seconds < 0:
        return "unknown"
    return f"{seconds:.2f}s"


def print_summary(metrics: Dict[str, Any], url: str) -> None:
    timestamp = datetime.now(timezone.utc).isoformat()
    state = metrics.get("state", "unknown")
    state_code = metrics.get("state_code")
    connected = metrics.get("connected")
    last_transition = format_timestamp(metrics.get("last_transition_us"))
    delta = format_delta(metrics.get("seconds_since_transition"))

    print(f"[{timestamp}] MQTT status from {url}")
    print(f"  State: {state} (code={state_code})")
    print(f"  Connected: {connected}")
    print(f"  Should run: {metrics.get('should_run')}")
    print(f"  Client running: {metrics.get('client_running')}")
    print(f"  Supervisor running: {metrics.get('supervisor_running')}")
    print(f"  Reconnect count: {metrics.get('reconnect_count')} (consecutive failures: {metrics.get('consecutive_failures')})")
    print(f"  Last transition: {last_transition} ({delta} ago)")
    print()


def main() -> int:
    args = parse_args()
    url = build_url(args.host, args.scheme, args.port)
    interval = args.watch if args.watch and args.watch > 0 else None

    while True:
        try:
            metrics = fetch_metrics(url, timeout=args.timeout, insecure=args.insecure)
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
                print(json.dumps(metrics, indent=2, sort_keys=True))
            else:
                print_summary(metrics, url)

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
