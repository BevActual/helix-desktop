#!/usr/bin/env python3
"""A scripted org.helix.Desktop for tests that must not touch real devices (Wi-Fi). It registers the interfaces of
protocols/dbus/org.helix.Desktop.xml on the session bus it is started on, serves a fixed Network1 world (a joined saved network, an open
one, a secured one never joined, an enterprise one, one VPN), changes that world when actions arrive, emits Changed, and appends every
action it receives to <state>/fake-network-actions.log as `ACTION <name> <json>`. It refuses what the real service would refuse (joining
a secured, unsaved network without a secret).

  fake-desktop.py <state dir>
"""
import json, os, sys, threading
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
state_dir = sys.argv[1]
log_path = os.path.join(state_dir, "fake-network-actions.log")
bt_log_path = os.path.join(state_dir, "fake-bluetooth-actions.log")
settings_log_path = os.path.join(state_dir, "fake-settings-actions.log")
settings = {"theme.mode": "dark", "nightlight.enabled": False, "theme.pure_black_dark": False}
NAME, PATH = "org.helix.Desktop", "/org/helix/Desktop"

world = {
    "enabled": True, "connected": True, "ssid": "Home WiFi", "scanning": False,
    "aps": {
        "/ap/home":  {"ssid": "Home WiFi", "strength": 71, "secured": True,  "enterprise": False, "saved": True},
        "/ap/cafe":  {"ssid": "Cafe",      "strength": 80, "secured": False, "enterprise": False, "saved": False},
        "/ap/neigh": {"ssid": "Neighbour", "strength": 55, "secured": True,  "enterprise": False, "saved": False},
        "/ap/corp":  {"ssid": "Corp",      "strength": 40, "secured": True,  "enterprise": True,  "saved": False},
    },
    "active": "/ap/home",
    "vpn": {"/vpn/work": {"name": "Work VPN", "active": False}},
}

def variant(value):
    if isinstance(value, bool): return GLib.Variant("b", value)
    if isinstance(value, int): return GLib.Variant("t", value)
    if isinstance(value, float): return GLib.Variant("d", value)
    if isinstance(value, str): return GLib.Variant("s", value)
    if isinstance(value, list): return GLib.Variant("av", [variant(v) for v in value])
    if isinstance(value, dict): return GLib.Variant("a{sv}", {k: variant(v) for k, v in value.items()})
    raise TypeError(type(value))

def network_snapshot():
    aps = [{"path": p, "device": "/dev/wlan0", "ssid": a["ssid"], "strength": a["strength"], "secured": a["secured"],
            "enterprise": a["enterprise"], "key-management": 2, "active": p == world["active"] and world["connected"],
            "frequency": 5180, "saved": a["saved"]} for p, a in world["aps"].items()]
    return {"available": True, "enabled": world["enabled"], "connected": world["connected"], "resolving": False,
            "scanning": world["scanning"], "ssid": world["ssid"], "address": "192.168.1.5", "interface": "wlan0", "kind": 1,
            "signal": world["aps"][world["active"]]["strength"] if world["connected"] else 0, "frequency": 5180,
            "vpn-active": False, "vpn-connected": False, "cellular-active": False, "wired": False, "cellular": False,
            "access-points": aps if world["enabled"] else [],
            "vpns": [{"path": p, "name": v["name"], "active": v["active"]} for p, v in world["vpn"].items()]}

bt = {
    "enabled": True, "scanning": False,
    "devices": {
        "/d/phone":   {"name": "Phone",           "paired": True,  "connected": False, "battery": None},
        "/d/buds":    {"name": "Earbuds",         "paired": True,  "connected": True,  "battery": 80},
        "/d/kbd":     {"name": "Keyboard",        "paired": True,  "connected": False, "battery": None},
        "/d/speaker": {"name": "Kitchen Speaker", "paired": False, "connected": False, "battery": None},
    },
}

def bluetooth_snapshot():
    devices = [{"path": p, "address": "AA:BB:CC:DD:EE:%02X" % i, "name": d["name"], "kind": 1, "paired": d["paired"],
                "trusted": d["paired"], "connected": d["connected"], "connecting": False, "has-battery": d["battery"] is not None,
                "battery": d["battery"] or 0, "has-rssi": False, "rssi": 0} for i, (p, d) in enumerate(bt["devices"].items())]
    return {"available": True, "enabled": bt["enabled"], "scanning": bt["scanning"], "name": "fake-laptop", "discoverable": False,
            "pairable": True, "soft-blocked": not bt["enabled"], "hard-blocked": False, "devices": devices if bt["enabled"] else []}

class Refused(Exception): pass

def bluetooth_action(name, args, emit):
    if name == "enable":
        bt["enabled"] = bool(args["value"])
        if not bt["enabled"]:
            bt["scanning"] = False
            for d in bt["devices"].values(): d["connected"] = False
    elif name in ("scan", "stop-scan"):
        if not bt["enabled"]: raise Refused("Bluetooth is off")
        bt["scanning"] = name == "scan"
    elif name in ("connect", "disconnect", "forget", "pair"):
        device = bt["devices"].get(args.get("path"))
        if not device or not bt["enabled"]: raise Refused("Device is no longer available")
        if name == "pair": raise Refused("Pairing was rejected")
        if name == "connect":
            if not device["paired"]: raise Refused("Connection rejected")
            device["connected"] = True
        elif name == "disconnect": device["connected"] = False
        elif name == "forget": device["paired"], device["connected"] = False, False
    else:
        raise Refused("Unknown Bluetooth action")

def network_action(name, args, emit):
    if name == "enable":
        world["enabled"] = bool(args["value"])
        if not world["enabled"]: world["connected"], world["ssid"] = False, ""
    elif name == "scan":
        if not world["enabled"]: raise Refused("Wi-Fi is off")
        world["scanning"] = True
        def finish():
            world["scanning"] = False; emit(); return False
        GLib.timeout_add(300, finish)
    elif name == "connect":
        ap = world["aps"].get(args["path"])
        if not ap or not world["enabled"]: raise Refused("Network is no longer available")
        if ap["secured"] and not ap["saved"] and "secret" not in args: raise Refused("Connection was rejected")
        if ap["enterprise"]: raise Refused("Connection was rejected")
        world.update(connected=True, ssid=ap["ssid"], active=args["path"])
    elif name == "disconnect":
        world["connected"], world["ssid"] = False, ""
    elif name == "forget":
        for path, ap in world["aps"].items():
            if ap["ssid"] == args["ssid"]:
                ap["saved"] = False
                if world["active"] == path and world["connected"]: world["connected"], world["ssid"] = False, ""
    elif name in ("vpn-connect", "vpn-disconnect"):
        vpn = world["vpn"].get(args["path"])
        if not vpn: raise Refused("Unknown VPN")
        vpn["active"] = name == "vpn-connect"
    else:
        raise Refused("Unknown network action")

next_operation = [100]
connection = None

def emit_changed(interface):
    connection.emit_signal(None, PATH, interface, "Changed", None)

def on_method(conn, sender, path, interface, method, params, invocation):
    short = interface.split(".")[-1]
    try:
        if method == "GetSnapshot":
            if short == "Network1": reply = network_snapshot()
            elif short == "Bluetooth1": reply = bluetooth_snapshot()
            else: reply = {"available": False}
            invocation.return_value(GLib.Variant("(a{sv})", (variant(reply).unpack() and {k: variant(v) for k, v in reply.items()},)))
        elif method == "GetSettings" and short == "Configuration1":
            def entry(**fields): return GLib.Variant("a{sv}", {k: variant(v) for k, v in fields.items()})
            entries = [entry(id="audio.sound_volume", type="number", value=0.5),
                       entry(id="theme.mode", title="Appearance", group="Colors", type="mode", value=settings["theme.mode"], options=["dark", "light", "auto"], **{"option-labels": ["Dark", "Light", "Automatic"]}),
                       entry(id="theme.pure_black_dark", title="Pure black in dark mode", group="Colors", description="Darker surfaces on OLED screens.", type="boolean", value=settings["theme.pure_black_dark"]),
                       entry(id="nightlight.enabled", title="Night Light", group="Night Light", type="boolean", value=settings["nightlight.enabled"])]
            invocation.return_value(GLib.Variant("(av)", (entries,)))
        elif method == "Update" and short == "Configuration1":
            setting_id, value = params.unpack()
            if isinstance(value, GLib.Variant): value = value.unpack()
            with open(settings_log_path, "a") as log: log.write(f"UPDATE {setting_id} {json.dumps(value)}\n")
            ok = (setting_id == "theme.mode" and value in ("dark", "light", "auto")) or (setting_id in ("nightlight.enabled", "theme.pure_black_dark") and isinstance(value, bool))
            if not ok:
                with open(settings_log_path, "a") as log: log.write(f"REFUSED {setting_id} {json.dumps(value)}\n")
                invocation.return_dbus_error("org.helix.Desktop.Error.InvalidArgument", "Setting could not be saved"); return
            settings[setting_id] = value
            invocation.return_value(None)
            GLib.idle_add(lambda: (emit_changed("org.helix.Desktop.Configuration1"), False)[1])
        elif method == "Action" and short == "Network1":
            name, args = params.unpack()
            args = {k: (v if not isinstance(v, GLib.Variant) else v.unpack()) for k, v in args.items()}
            shown = {k: (f"<secret:{len(v)}>" if k == "secret" else v) for k, v in args.items()}   # a password never goes in a log
            with open(log_path, "a") as log: log.write(f"ACTION {name} {json.dumps(shown, sort_keys=True)}\n")
            try:
                network_action(name, args, lambda: emit_changed("org.helix.Desktop.Network1"))
            except (Refused, KeyError) as error:
                with open(log_path, "a") as log: log.write(f"REFUSED {name} {error}\n")
                invocation.return_dbus_error("org.helix.Desktop.Error.Failed", str(error)); return
            next_operation[0] += 1
            invocation.return_value(GLib.Variant("(t)", (next_operation[0],)))
            GLib.idle_add(lambda: (emit_changed("org.helix.Desktop.Network1"), False)[1])
        elif method == "Action" and short == "Bluetooth1":
            name, args = params.unpack()
            args = {k: (v if not isinstance(v, GLib.Variant) else v.unpack()) for k, v in args.items()}
            with open(bt_log_path, "a") as log: log.write(f"ACTION {name} {json.dumps(args, sort_keys=True)}\n")
            try:
                bluetooth_action(name, args, lambda: emit_changed("org.helix.Desktop.Bluetooth1"))
            except (Refused, KeyError) as error:
                with open(bt_log_path, "a") as log: log.write(f"REFUSED {name} {error}\n")
                invocation.return_dbus_error("org.helix.Desktop.Error.Failed", str(error)); return
            next_operation[0] += 1
            invocation.return_value(GLib.Variant("(t)", (next_operation[0],)))
            GLib.idle_add(lambda: (emit_changed("org.helix.Desktop.Bluetooth1"), False)[1])
        else:
            invocation.return_dbus_error("org.helix.Desktop.Error.Failed", f"fake service: {short}.{method} is not implemented")
    except Exception as error:  # a bug in this fake must be loud, not a silent timeout
        invocation.return_dbus_error("org.helix.Desktop.Error.Failed", f"fake service error: {error}")

def on_bus(conn, name):
    global connection
    connection = conn
    xml = open(os.path.join(ROOT, "protocols/dbus/org.helix.Desktop.xml")).read()
    node = Gio.DBusNodeInfo.new_for_xml(xml)
    for iface in node.interfaces:
        conn.register_object(PATH, iface, on_method, None, None)
    print("fake desktop service ready", flush=True)

Gio.bus_own_name(Gio.BusType.SESSION, NAME, Gio.BusNameOwnerFlags.NONE, on_bus, None, None)
GLib.MainLoop().run()
