#!/usr/bin/env python3
"""WebSocket diagnostics client for the KC-Device dashboard.

This helper connects to /ws/sensors (or a custom path), decodes the
JSON frames emitted by the firmware (status snapshots, focus status, and
focus samples), and prints readable summaries. Use it to verify live
sensor streaming without opening the ESP-IDF monitor.

Requires the `websockets` package: pip install websockets
"""

from __future__ import annotations

import argparse
import asyncio
import json
import ssl
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any, Dict, Optional

try:
    import websockets
except ImportError as exc:  # pragma: no cover - user environment check
    print(
        "The ws_diag tool needs the 'websockets' package. Install it with 'pip install websockets'.",
        file=sys.stderr,
    )
    raise

DEFAULT_HOST = "kc-device.local"
DEFAULT_PATH = "/ws/sensors"


@dataclass
class Stats:
    status_frames: int = 0
    focus_status_frames: int = 0
    focus_samples: int = 0
    last_snapshot_ms: Optional[float] = None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Stream WebSocket diagnostics from the device dashboard",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("host", nargs="?", default=DEFAULT_HOST, help="Device hostname or IP")
    parser.add_argument("--port", type=int, default=None, help="Port for the WebSocket server")
    parser.add_argument(
        "--scheme",
        choices=("wss", "ws"),
        default="wss",
        help="WebSocket scheme (wss uses TLS)",
    )
    parser.add_argument("--path", default=DEFAULT_PATH, help="WebSocket path on the device")
    parser.add_argument(
        "--insecure",
        action="store_true",
        help="Skip TLS verification when using wss",
    )
    parser.add_argument(
        "--request-snapshot",
        action="store_true",
        help="Send the request_snapshot action after connecting",
    )
    parser.add_argument(
        "--focus-address",
        metavar="ADDR",
        type=parse_i2c_address,
        help="Start focus streaming for the specified I2C address (decimal or 0x-prefixed)",
    )
    parser.add_argument(
        "--focus-duration",
        type=float,
        default=0.0,
        help="Automatically stop focus mode after N seconds (0 disables)",
    )
    parser.add_argument(
        "--raw",
        action="store_true",
        help="Print raw JSON instead of summaries",
    )
    parser.add_argument(
        "--no-reconnect",
        action="store_true",
        help="Exit immediately on disconnect instead of retrying",
    )
    parser.add_argument(
        "--retry-wait",
        type=float,
        default=3.0,
        help="Seconds to wait before reconnecting",
    )
    return parser.parse_args()


def parse_i2c_address(value: str) -> int:
    try:
        normalized = value.strip().lower()
        base = 16 if normalized.startswith("0x") else 10
        parsed = int(normalized, base)
    except (ValueError, AttributeError) as exc:
        raise argparse.ArgumentTypeError(f"Invalid I2C address: {value}") from exc

    if not (0 <= parsed <= 0x7F):
        raise argparse.ArgumentTypeError("I2C address must be within 0x00-0x7F")
    return parsed


def build_ws_url(host: str, scheme: str, port: Optional[int], path: str) -> str:
    default_port = 80 if scheme == "ws" else 443
    final_port = port or default_port
    if not path.startswith("/"):
        path = "/" + path
    return f"{scheme}://{host}:{final_port}{path}"


def create_ssl_context(args: argparse.Namespace) -> Optional[ssl.SSLContext]:
    if args.scheme != "wss":
        return None
    context = ssl.create_default_context()
    if args.insecure:
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
    return context


def format_timestamp_ms(ms: Optional[float]) -> str:
    if ms is None:
        return "?"
    seconds = ms / 1000.0
    dt = datetime.fromtimestamp(seconds, tz=timezone.utc)
    return dt.isoformat()


def format_address(addr: Any) -> str:
    if isinstance(addr, int):
        return f"0x{addr:02X}"
    return "unknown"


def log_status_snapshot(payload: Dict[str, Any], stats: Stats) -> None:
    stats.status_frames += 1
    stats.last_snapshot_ms = payload.get("timestamp_ms")
    sensors = payload.get("sensors")
    sensor_count = len(sensors) if isinstance(sensors, dict) else 0
    battery = payload.get("battery")
    rssi = payload.get("rssi")
    timestamp = format_timestamp_ms(payload.get("timestamp_ms"))
    print(
        f"[status_snapshot #{stats.status_frames}] {sensor_count} sensor entries, battery={battery}%, rssi={rssi} dBm, timestamp={timestamp}"
    )


def log_focus_status(payload: Dict[str, Any], stats: Stats) -> None:
    stats.focus_status_frames += 1
    status = payload.get("status")
    address = payload.get("address")
    print(f"[focus_status #{stats.focus_status_frames}] address={format_address(address)} status={status}")


def log_focus_sample(payload: Dict[str, Any], stats: Stats) -> None:
    stats.focus_samples += 1
    sensor = payload.get("sensor") or {}
    address = sensor.get("address")
    name = sensor.get("name")
    sensor_type = sensor.get("type")
    reading = sensor.get("reading")
    raw = sensor.get("raw")
    timestamp = format_timestamp_ms(payload.get("timestamp_ms"))
    print(
        f"[focus_sample #{stats.focus_samples}] addr={format_address(address)} ({name}/{sensor_type}) timestamp={timestamp} reading={reading or raw}"
    )


def print_raw(payload: Dict[str, Any]) -> None:
    print(json.dumps(payload, indent=2, sort_keys=True))


async def send_command(ws: websockets.WebSocketClientProtocol, payload: Dict[str, Any]) -> None:
    await ws.send(json.dumps(payload))


async def handle_messages(
    ws: websockets.WebSocketClientProtocol,
    args: argparse.Namespace,
    stats: Stats,
) -> None:
    focus_address = args.focus_address
    focus_stop_time: Optional[float] = None

    if args.request_snapshot:
        await send_command(ws, {"action": "request_snapshot"})

    if focus_address is not None:
        await send_command(ws, {"action": "focus_start", "address": focus_address})
        if args.focus_duration > 0:
            focus_stop_time = time.monotonic() + args.focus_duration

    async for message in ws:
        try:
            payload = json.loads(message)
        except json.JSONDecodeError:
            print(f"[raw] {message}")
            continue

        if args.raw:
            print_raw(payload)
            continue

        msg_type = payload.get("type")
        if msg_type == "status_snapshot":
            log_status_snapshot(payload, stats)
        elif msg_type == "focus_status":
            log_focus_status(payload, stats)
        elif msg_type == "focus_sample":
            log_focus_sample(payload, stats)
        else:
            print(f"[unknown] {payload}")

        if focus_stop_time is not None and time.monotonic() >= focus_stop_time:
            await send_command(ws, {"action": "focus_stop"})
            focus_stop_time = None


async def run_diag(args: argparse.Namespace) -> int:
    url = build_ws_url(args.host, args.scheme, args.port, args.path)
    ssl_context = create_ssl_context(args)
    stats = Stats()

    print(f"Connecting to {url} (insecure TLS={args.insecure})")

    while True:
        try:
            async with websockets.connect(
                url,
                ssl=ssl_context,
                ping_interval=20,
                ping_timeout=10,
                close_timeout=5,
            ) as ws:
                print("Connected. Awaiting frames...")
                await handle_messages(ws, args, stats)
        except websockets.exceptions.ConnectionClosedOK:
            print("Connection closed by server")
            if args.no_reconnect:
                return 0
        except asyncio.CancelledError:
            raise
        except Exception as exc:  # pragma: no cover - runtime diagnostics
            print(f"Connection error: {exc}", file=sys.stderr)
            if args.no_reconnect:
                return 2

        print(f"Reconnecting in {args.retry_wait} seconds...")
        await asyncio.sleep(args.retry_wait)


def main() -> int:
    args = parse_args()
    try:
        return asyncio.run(run_diag(args))
    except KeyboardInterrupt:
        print("Interrupted by user")
        return 0


if __name__ == "__main__":
    sys.exit(main())
