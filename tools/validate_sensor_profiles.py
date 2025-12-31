#!/usr/bin/env python3
"""Validate sensor profile definitions before flashing firmware."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Dict, List, Sequence, Tuple

ALLOWED_SENSOR_TYPES: Sequence[str] = (
    "EZO_PH",
    "EZO_EC",
    "EZO_RTD",
    "EZO_DO",
    "EZO_ORP",
    "EZO_HUM",
)

ALLOWED_CHANNELS: Dict[str, Tuple[str, ...]] = {
    "EZO_EC": ("conductivity", "tds", "salinity", "specific_gravity"),
    "EZO_HUM": ("humidity", "air_temp", "dew_point"),
    "EZO_DO": ("dissolved_oxygen", "saturation"),
    "EZO_RTD": ("temperature",),
    "EZO_ORP": ("orp",),
    "EZO_PH": ("ph",),
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate sensor profile JSON files.")
    parser.add_argument(
        "--profiles",
        default="config/runtime/sensor_profiles.json",
        help="Path to the sensor profile JSON file (default: %(default)s)",
    )
    return parser.parse_args()


def normalize_address(raw_value) -> str:
    if isinstance(raw_value, int):
        if raw_value < 0 or raw_value > 0x7F:
            raise ValueError("address int out of 7-bit range")
        return f"0x{raw_value:02X}"

    if isinstance(raw_value, str):
        value = raw_value.strip()
        base = 16 if value.lower().startswith("0x") else 10
        parsed = int(value, base)
        if parsed < 0 or parsed > 0x7F:
            raise ValueError("address value out of 7-bit range")
        return f"0x{parsed:02X}"

    raise ValueError("address must be int or hex string")


def validate_profile(profile: dict, index: int) -> Tuple[List[str], int]:
    errors: List[str] = []
    profile_name = profile.get("name")
    if not profile_name or not isinstance(profile_name, str):
        errors.append(f"Profile #{index + 1} missing string 'name' field")
        profile_name = f"<unnamed:{index + 1}>"

    sensors = profile.get("sensors")
    if not isinstance(sensors, list) or not sensors:
        errors.append(f"Profile '{profile_name}' must contain a non-empty 'sensors' list")
        return errors, 0

    seen_addresses = set()
    sensor_count = 0

    for sensor_index, sensor in enumerate(sensors):
        sensor_count += 1
        context = f"Profile '{profile_name}' sensor #{sensor_index + 1}"

        if not isinstance(sensor, dict):
            errors.append(f"{context}: expected object, found {type(sensor).__name__}")
            continue

        sensor_type = sensor.get("type")
        if sensor_type not in ALLOWED_SENSOR_TYPES:
            errors.append(
                f"{context}: invalid type '{sensor_type}'. Allowed: {', '.join(ALLOWED_SENSOR_TYPES)}"
            )

        try:
            address = normalize_address(sensor.get("address"))
        except Exception as exc:
            errors.append(f"{context}: invalid address '{sensor.get('address')}': {exc}")
            continue

        if address in seen_addresses:
            errors.append(f"{context}: duplicate I2C address {address} within profile")
        else:
            seen_addresses.add(address)

        bus = sensor.get("bus")
        if bus is not None and not isinstance(bus, str):
            errors.append(f"{context}: bus must be a string when provided")

        channels = sensor.get("channels", [])
        if channels is None:
            channels = []
        if not isinstance(channels, list):
            errors.append(f"{context}: channels must be a list when provided")
            continue

        seen_channels = set()
        allowed = set(ALLOWED_CHANNELS.get(sensor_type, ())) or None
        for channel in channels:
            if not isinstance(channel, str):
                errors.append(f"{context}: channel entries must be strings")
                continue
            if channel in seen_channels:
                errors.append(f"{context}: duplicate channel '{channel}'")
            else:
                seen_channels.add(channel)
            if allowed is not None and channel not in allowed:
                errors.append(
                    f"{context}: unsupported channel '{channel}' for type {sensor_type}"
                )

    return errors, sensor_count


def main() -> int:
    args = parse_args()
    profiles_path = Path(args.profiles)

    if not profiles_path.exists():
        print(f"[ERROR] Profile file not found: {profiles_path}", file=sys.stderr)
        return 2

    try:
        data = json.loads(profiles_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        print(
            f"[ERROR] Failed to parse {profiles_path}: line {exc.lineno} column {exc.colno}: {exc.msg}",
            file=sys.stderr,
        )
        return 2

    profiles = data.get("profiles")
    if not isinstance(profiles, list) or not profiles:
        print("[ERROR] JSON must contain a non-empty 'profiles' list", file=sys.stderr)
        return 1

    all_errors: List[str] = []
    total_sensors = 0
    for index, profile in enumerate(profiles):
        profile_errors, sensor_count = validate_profile(profile, index)
        total_sensors += sensor_count
        all_errors.extend(profile_errors)

    if all_errors:
        for error in all_errors:
            print(f"[ERROR] {error}", file=sys.stderr)
        return 1

    print(
        f"Validated {len(profiles)} profile(s) covering {total_sensors} sensor entries in {profiles_path}."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
