#!/usr/bin/env python3
"""Exercise the service capture lease and reconnect on a hardware-free PipeWire server."""
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from gi.repository import Gio, GLib

service_binary, server_config = map(os.path.abspath, sys.argv[1:])
service_tag = Path(service_binary).name
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
levels = []
bus.signal_subscribe("org.helix.Desktop", "org.helix.Desktop.Audio1", "InputLevel",
                     "/org/helix/Desktop", None, 0,
                     lambda *args: levels.append(args[5].unpack()), None)

def call(method, args=None, connection=bus):
    return connection.call_sync("org.helix.Desktop", "/org/helix/Desktop",
                                "org.helix.Desktop.Audio1", method, args, None, 0, 2000, None).unpack()

def wait_for_bus_name():
    probe = Gio.DBusConnection.new_for_address_sync(
        os.environ["DBUS_SESSION_BUS_ADDRESS"],
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
        | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
        None,
        None,
    )
    for _ in range(100):
        owner = probe.call_sync(
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus",
            "NameHasOwner",
            GLib.Variant("(s)", ("org.helix.Desktop",)),
            None,
            Gio.DBusCallFlags.NONE,
            1000,
            None,
        )
        if owner[0]:
            probe.close_sync(None)
            return
        time.sleep(0.1)
    probe.close_sync(None)
    raise AssertionError("desktop service did not claim org.helix.Desktop")

def _append_pw_objects(nodes, value):
    if isinstance(value, list):
        for item in value:
            _append_pw_objects(nodes, item)
    elif isinstance(value, dict):
        nodes.append(value)


def pw_dump_nodes(raw):
    """Parse `pw-dump` output (array, NDJSON, or concatenated JSON values)."""
    raw = raw.strip()
    if not raw:
        return []
    nodes = []
    try:
        _append_pw_objects(nodes, json.loads(raw))
        if nodes:
            return nodes
    except json.JSONDecodeError:
        pass
    decoder = json.JSONDecoder()
    index = 0
    while index < len(raw):
        while index < len(raw) and raw[index].isspace():
            index += 1
        if index >= len(raw):
            break
        try:
            value, end = decoder.raw_decode(raw, index)
        except json.JSONDecodeError:
            break
        _append_pw_objects(nodes, value)
        index += end
    if nodes:
        return nodes
    for line in raw.splitlines():
        line = line.strip().rstrip(",")
        if not line or line in ("[", "]", "{", "}"):
            continue
        try:
            _append_pw_objects(nodes, json.loads(line))
        except json.JSONDecodeError:
            continue
    return nodes

def until(predicate, seconds=8):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        while GLib.MainContext.default().pending():
            GLib.MainContext.default().iteration(False)
        try:
            if predicate():
                return
        except GLib.Error:
            pass
        time.sleep(.05)
    raise AssertionError("Timed out waiting for the audio test state")

with tempfile.TemporaryDirectory(prefix="helix-meter-") as temporary:
    root = Path(temporary)
    env = dict(os.environ, PIPEWIRE_RUNTIME_DIR=str(root), PIPEWIRE_REMOTE="helix-meter-test",
               DBUS_SYSTEM_BUS_ADDRESS=os.environ["DBUS_SESSION_BUS_ADDRESS"], GSETTINGS_BACKEND="memory")
    for role in ("CONFIG", "STATE", "DATA", "CACHE"):
        env[f"XDG_{role}_HOME"] = str(root / role.lower())
        env[f"HELIX_{role}_HOME"] = str(root / role.lower())
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    daemon = None
    with (root / "service.log").open("w") as log:
        service = subprocess.Popen([service_binary], env=env, stdout=log, stderr=log)
        try:
            wait_for_bus_name()
            until(lambda: not call("GetSnapshot")[0]["available"])
            def start_server():
                return subprocess.Popen(["pipewire", "-c", server_config], env=env,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            daemon = start_server()
            until(lambda: call("GetSnapshot")[0].get("sources"))
            source = call("GetSnapshot")[0]["sources"][0]["id"]
            call("StartInputMeter", GLib.Variant("(u)", (source,)))
            def command(*args):
                return subprocess.check_output(args, env=env, text=True)

            def pw_dump_text():
                try:
                    return command("pw-dump")
                except subprocess.CalledProcessError:
                    return ""

            def capture_nodes():
                out = []
                for node in pw_dump_nodes(pw_dump_text()):
                    if not isinstance(node, dict):
                        continue
                    info = node.get("info")
                    if not isinstance(info, dict):
                        continue
                    props = info.get("props")
                    if not isinstance(props, dict):
                        continue
                    if props.get("media.name") == "Helix microphone level":
                        out.append(node)
                return out
            until(capture_nodes)
            for node in capture_nodes():
                command("pw-cli", "set-param", str(node["id"]), "PortConfig",
                        "{ direction = Input mode = dsp format = { mediaType = audio mediaSubtype = raw format = F32 rate = 48000 channels = 1 position = [ MONO ] } }")
            until(lambda: service_tag in command("pw-link", "-i"))
            for output in command("pw-link", "-o").splitlines():
                if "Helix-Test-Source" in output:
                    for input_port in command("pw-link", "-i").splitlines():
                        if service_tag in input_port:
                            command("pw-link", output, input_port)
            until(lambda: any(available and math.isfinite(peak) and 0 <= peak <= 1
                              for _, peak, available in levels))
            call("StopInputMeter")
            until(lambda: not capture_nodes())
            # No lease renewal: a hung client must release the microphone automatically.
            call("StartInputMeter", GLib.Variant("(u)", (source,)))
            until(capture_nodes)
            until(lambda: not capture_nodes(), seconds=15)
            # A vanished client releases its own stream, without stopping other clients.
            peer = Gio.DBusConnection.new_for_address_sync(os.environ["DBUS_SESSION_BUS_ADDRESS"],
                    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
                    None, None)
            call("StartInputMeter", GLib.Variant("(u)", (source,)), peer)
            until(capture_nodes)
            peer.close_sync(None)
            until(lambda: not capture_nodes())
            daemon.terminate(); daemon.wait(timeout=5); daemon = None
            until(lambda: not call("GetSnapshot")[0]["available"])
            daemon = start_server()
            until(lambda: call("GetSnapshot")[0].get("sources"))
            assert service.poll() is None
            print("Synthetic microphone levels, stop, lease expiry, disconnect and PipeWire recovery verified")
        except Exception:
            log.flush()
            print((root / "service.log").read_text(), file=sys.stderr)
            raise
        finally:
            service.terminate(); service.wait(timeout=5)
            if daemon:
                daemon.terminate(); daemon.wait(timeout=5)
