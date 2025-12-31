"""Validate runtime configuration JSON blobs used by the firmware.

This script mirrors the constraints enforced by build.ps1 so contributors can
lint configuration updates without running a full ESP-IDF build. It checks the
embedded service toggles, MQTT settings, dashboard metadata, time-sync settings,
and optional API-key lists.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

RE_HOSTNAME = re.compile(r"^[a-z0-9-]{1,32}$")


class ValidationError(Exception):
    pass


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ValidationError(f"Missing file: {path}") from exc
    except json.JSONDecodeError as exc:
        raise ValidationError(f"Invalid JSON in {path}: {exc}") from exc


def ensure_keys(obj: Dict[str, Any], keys: List[str], *, ctx: str) -> None:
    missing = [key for key in keys if key not in obj]
    if missing:
        raise ValidationError(f"{ctx}: missing keys {', '.join(missing)}")


def validate_services_config(config: Dict[str, Any], *, ctx: str) -> None:
    ensure_keys(
        config,
        [
            "enable_http_server",
            "enable_mqtt",
            "enable_mdns",
            "enable_time_sync",
            "https_port",
        ],
        ctx=f"{ctx}.services",
    )
    https_port = config["https_port"]
    if not isinstance(https_port, int) or https_port <= 0:
        raise ValidationError(f"{ctx}.services.https_port must be a positive integer")


def validate_dashboard_config(config: Dict[str, Any], *, ctx: str) -> None:
    ensure_keys(config, ["mdns_hostname", "mdns_instance_name"], ctx=f"{ctx}.dashboard")
    hostname = config["mdns_hostname"]
    instance = config["mdns_instance_name"]
    if not isinstance(hostname, str) or not RE_HOSTNAME.fullmatch(hostname):
        raise ValidationError(
            f"{ctx}.dashboard.mdns_hostname must be 1-32 lowercase letters/numbers/hyphens",
        )
    if not isinstance(instance, str) or not (1 <= len(instance) <= 63):
        raise ValidationError(
            f"{ctx}.dashboard.mdns_instance_name must be 1-63 characters",
        )


def validate_mqtt_config(config: Dict[str, Any], *, ctx: str) -> None:
    ensure_keys(config, ["broker_uri", "username", "password"], ctx=f"{ctx}.mqtt")
    broker_uri = config["broker_uri"]
    username = config["username"]
    password = config["password"]
    for field_name, value in ("broker_uri", broker_uri), ("username", username), ("password", password):
        if not isinstance(value, str) or not value.strip():
            raise ValidationError(f"{ctx}.mqtt.{field_name} must be a non-empty string")


def validate_telemetry_config(config: Dict[str, Any], *, ctx: str) -> None:
    ensure_keys(
        config,
        [
            "publish_interval_sec",
            "busy_backoff_ms",
            "client_backoff_ms",
            "idle_delay_ms",
        ],
        ctx=f"{ctx}.telemetry",
    )
    publish_interval = config["publish_interval_sec"]
    busy_backoff = config["busy_backoff_ms"]
    client_backoff = config["client_backoff_ms"]
    idle_delay = config["idle_delay_ms"]

    if not isinstance(publish_interval, int) or not (0 <= publish_interval <= 86400):
        raise ValidationError(f"{ctx}.telemetry.publish_interval_sec must be between 0 and 86400 seconds")
    for field_name, value in (
        ("busy_backoff_ms", busy_backoff),
        ("client_backoff_ms", client_backoff),
        ("idle_delay_ms", idle_delay),
    ):
        if not isinstance(value, int) or not (0 <= value <= 600000):
            raise ValidationError(f"{ctx}.telemetry.{field_name} must be between 0 and 600000 milliseconds")


def validate_time_sync_config(config: Dict[str, Any], *, ctx: str) -> None:
    ensure_keys(config, ["timezone", "timeout_sec"], ctx=f"{ctx}.time_sync")
    timezone = config["timezone"]
    timeout = config["timeout_sec"]
    if not isinstance(timezone, str) or not timezone.strip():
        raise ValidationError(f"{ctx}.time_sync.timezone must be a non-empty string")
    if not isinstance(timeout, int) or not (1 <= timeout <= 120):
        raise ValidationError(f"{ctx}.time_sync.timeout_sec must be between 1 and 120 seconds")

    retry_attempts = config.get("retry_attempts")
    if retry_attempts is not None:
        if not isinstance(retry_attempts, int) or not (0 <= retry_attempts <= 5):
            raise ValidationError(f"{ctx}.time_sync.retry_attempts must be between 0 and 5")

    retry_delay = config.get("retry_delay_sec")
    if retry_delay is not None:
        if not isinstance(retry_delay, int) or not (1 <= retry_delay <= 300):
            raise ValidationError(f"{ctx}.time_sync.retry_delay_sec must be between 1 and 300 seconds")


def validate_services_blob(data: Dict[str, Any], *, ctx: str = "services.json") -> None:
    if "services" not in data or not isinstance(data["services"], dict):
        raise ValidationError(f"{ctx}: missing 'services' object")
    services_cfg = data["services"]
    validate_services_config(services_cfg, ctx=ctx)

    enable_http = bool(services_cfg["enable_http_server"]) or bool(services_cfg["enable_mdns"])
    enable_mqtt = bool(services_cfg["enable_mqtt"])
    enable_time_sync = bool(services_cfg["enable_time_sync"])

    if enable_http:
        dashboard_cfg = data.get("dashboard")
        if not isinstance(dashboard_cfg, dict):
            raise ValidationError(f"{ctx}: HTTP/mDNS enabled but 'dashboard' object missing")
        validate_dashboard_config(dashboard_cfg, ctx=ctx)

    if enable_mqtt:
        mqtt_cfg = data.get("mqtt")
        if not isinstance(mqtt_cfg, dict):
            raise ValidationError(f"{ctx}: enable_mqtt is true but 'mqtt' object missing")
        validate_mqtt_config(mqtt_cfg, ctx=ctx)
        telemetry_cfg = data.get("telemetry")
        if not isinstance(telemetry_cfg, dict):
            raise ValidationError(f"{ctx}: enable_mqtt is true but 'telemetry' object missing")
        validate_telemetry_config(telemetry_cfg, ctx=ctx)

    if enable_time_sync:
        time_sync_cfg = data.get("time_sync")
        if not isinstance(time_sync_cfg, dict):
            raise ValidationError(f"{ctx}: enable_time_sync is true but 'time_sync' object missing")
        validate_time_sync_config(time_sync_cfg, ctx=ctx)


def validate_api_keys_blob(data: Dict[str, Any], *, ctx: str = "api_keys.json") -> None:
    keys = data.get("api_keys")
    if keys is None:
        raise ValidationError(f"{ctx}: missing 'api_keys' array")
    if not isinstance(keys, list):
        raise ValidationError(f"{ctx}: 'api_keys' must be a list")
    for idx, entry in enumerate(keys):
        if not isinstance(entry, dict):
            raise ValidationError(f"{ctx}[{idx}]: expected object")
        ensure_keys(entry, ["name", "type", "value", "enabled"], ctx=f"{ctx}[{idx}]")
        if entry["type"] not in {"cloud", "dashboard", "custom"}:
            raise ValidationError(f"{ctx}[{idx}].type must be cloud|dashboard|custom")
        if not isinstance(entry["enabled"], bool):
            raise ValidationError(f"{ctx}[{idx}].enabled must be boolean")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate KC-Device runtime configuration JSON blobs")
    parser.add_argument(
        "--services",
        type=Path,
        default=Path("config/runtime/services.json"),
        help="Path to services.json (defaults to config/runtime/services.json)",
    )
    parser.add_argument(
        "--api-keys",
        type=Path,
        default=Path("config/runtime/api_keys.json"),
        help="Path to api_keys.json (optional). Validation skipped if file is missing and flag not provided.",
    )
    parser.add_argument(
        "--require-api-keys",
        action="store_true",
        help="Treat missing api_keys.json as an error instead of skipping",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        services_blob = load_json(args.services)
        validate_services_blob(services_blob, ctx=str(args.services))

        if args.api_keys.exists():
            api_keys_blob = load_json(args.api_keys)
            validate_api_keys_blob(api_keys_blob, ctx=str(args.api_keys))
        elif args.require_api_keys:
            raise ValidationError(f"Missing api_keys.json at {args.api_keys}")

    except ValidationError as exc:
        print(f"Validation failed: {exc}", file=sys.stderr)
        return 1

    print("Runtime configuration validated successfully")
    return 0


if __name__ == "__main__":
    sys.exit(main())
