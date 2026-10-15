#!/usr/bin/env python3
"""Capture and compare org.helix.Desktop snapshots across builds or machines.

  snapshot.py capture OUT.json     read-only: every domain's GetSnapshot + GetSettings
  snapshot.py diff A.json B.json   structural diff (keys, types, list lengths, stable scalars)

Capture twice (e.g. before/after a desktop change) on the same session, then `diff`.
Volatile fields (signal strength, levels, timestamps) are reported as type only.
Exit status is 1 when the structures differ.
"""
import json
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

BUS, PATH = "org.helix.Desktop", "/org/helix/Desktop"
DOMAINS = {
    "configuration": "Configuration1",
    "appearance": "Appearance1",
    "audio": "Audio1",
    "network": "Network1",
    "bluetooth": "Bluetooth1",
    "displays": "Displays1",
}
VOLATILE = {"signal", "strength", "rssi", "battery", "volume", "peak", "level", "frequency", "address"}


def call(bus, interface, method):
    reply = bus.call_sync(
        BUS, PATH, f"org.helix.Desktop.{interface}", method, None, None, Gio.DBusCallFlags.NONE, 5000, None
    )
    return reply.unpack()[0]


def capture(out):
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    result = {}
    for domain, interface in DOMAINS.items():
        try:
            result[domain] = {"snapshot": call(bus, interface, "GetSnapshot")}
        except GLib.Error as error:
            result[domain] = {"error": error.message}
    try:
        settings = call(bus, "Configuration1", "GetSettings")
        result["settings"] = {row["id"]: row for row in settings}
    except GLib.Error as error:
        result["settings"] = {"error": error.message}
    with open(out, "w") as handle:
        json.dump(result, handle, indent=1, sort_keys=True, default=str)
    print(f"captured {len(result.get('settings', {}))} settings, {len(DOMAINS)} domains -> {out}")


def shape(value):
    if isinstance(value, dict):
        return {k: shape(v) for k, v in value.items()}
    if isinstance(value, list):
        return [shape(value[0])] if value else []
    return type(value).__name__


def walk(a, b, path, problems):
    if isinstance(a, dict) and isinstance(b, dict):
        for key in sorted(set(a) | set(b)):
            if key not in a:
                problems.append(f"{path}.{key}: only in B")
            elif key not in b:
                problems.append(f"{path}.{key}: only in A")
            else:
                walk(a[key], b[key], f"{path}.{key}", problems)
        return
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b) and not path.endswith(("devices", "access-points", "sinks", "sources", "programs")):
            problems.append(f"{path}: length {len(a)} vs {len(b)}")
        for i, (x, y) in enumerate(zip(a, b)):
            walk(x, y, f"{path}[{i}]", problems)
        return
    if type(a) is not type(b) and not (isinstance(a, (int, float)) and isinstance(b, (int, float))):
        problems.append(f"{path}: type {type(a).__name__} vs {type(b).__name__}")
        return
    leaf = path.rsplit(".", 1)[-1]
    if a != b and leaf not in VOLATILE and not isinstance(a, float):
        problems.append(f"{path}: {a!r} vs {b!r}")


def diff(path_a, path_b):
    a, b = json.load(open(path_a)), json.load(open(path_b))
    problems = []
    for domain in sorted(set(a) | set(b)):
        if domain in ("configuration", "appearance"):
            # `implementation` legitimately differs; everything else must match.
            for side in (a, b):
                side.get(domain, {}).get("snapshot", {}).pop("implementation", None)
        walk(a.get(domain), b.get(domain), domain, problems)
    for line in problems:
        print(line)
    print(f"{len(problems)} difference(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "capture":
        capture(sys.argv[2])
    elif len(sys.argv) == 4 and sys.argv[1] == "diff":
        sys.exit(diff(sys.argv[2], sys.argv[3]))
    else:
        print(__doc__)
        sys.exit(2)
