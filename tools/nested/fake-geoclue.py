#!/usr/bin/env python3
"""A scripted GeoClue2 on a private bus, for the desktop service's automatic location. It owns org.freedesktop.GeoClue2 with the Manager (GetClient),
a Client (DesktopId, RequestedAccuracyLevel, DistanceThreshold, Start, Stop, the LocationUpdated signal) and a Location (Latitude, Longitude,
Description). Start answers with Paris after a moment, as GeoClue answers after its first lookup. HELIX_FAKE_GEOCLUE=deny makes Start fail the way
GeoClue does for a program it has not been told to trust. Every call is appended to <state dir>/fake-geoclue.log.

  fake-geoclue.py <state dir> <bus address>
"""
import os, sys
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

state_dir, address = sys.argv[1], sys.argv[2]
log_path = os.path.join(state_dir, "fake-geoclue.log")
DENY = os.environ.get("HELIX_FAKE_GEOCLUE") == "deny"
CLIENT = "/org/freedesktop/GeoClue2/Client/1"
LOCATION = "/org/freedesktop/GeoClue2/Client/1/Location/1"
props = {"DesktopId": "", "RequestedAccuracyLevel": 0, "DistanceThreshold": 0}
conn = None

def log(line):
    with open(log_path, "a") as handle: handle.write(line + "\n")

MANAGER_XML = """<node><interface name="org.freedesktop.GeoClue2.Manager"><method name="GetClient"><arg type="o" direction="out"/></method></interface></node>"""
CLIENT_XML = """<node><interface name="org.freedesktop.GeoClue2.Client">
<property name="DesktopId" type="s" access="readwrite"/><property name="RequestedAccuracyLevel" type="u" access="readwrite"/>
<property name="DistanceThreshold" type="u" access="readwrite"/><property name="Location" type="o" access="read"/>
<method name="Start"/><method name="Stop"/>
<signal name="LocationUpdated"><arg type="o"/><arg type="o"/></signal></interface></node>"""
LOCATION_XML = """<node><interface name="org.freedesktop.GeoClue2.Location">
<property name="Latitude" type="d" access="read"/><property name="Longitude" type="d" access="read"/>
<property name="Accuracy" type="d" access="read"/><property name="Description" type="s" access="read"/></interface></node>"""

def on_manager(c, sender, path, iface, method, params, inv):
    if method == "GetClient":
        log("GetClient")
        inv.return_value(GLib.Variant("(o)", (CLIENT,)))

def on_client(c, sender, path, iface, method, params, inv):
    if method == "Start":
        log(f"Start desktop_id={props['DesktopId']} accuracy={props['RequestedAccuracyLevel']} threshold={props['DistanceThreshold']}")
        if DENY:
            inv.return_dbus_error("org.freedesktop.DBus.Error.AccessDenied", "'org.helix.Desktop' disallowed, no agent for UID"); return
        inv.return_value(None)
        GLib.timeout_add(300, lambda: (c.emit_signal(None, CLIENT, "org.freedesktop.GeoClue2.Client", "LocationUpdated", GLib.Variant("(oo)", ("/", LOCATION))), False)[1])
    elif method == "Stop":
        log("Stop"); inv.return_value(None)

def client_get(c, sender, path, iface, name):
    if name in props:
        return GLib.Variant("s" if name == "DesktopId" else "u", props[name])
    if name == "Location": return GLib.Variant("o", "/")

def client_set(c, sender, path, iface, name, value):
    props[name] = value.unpack(); return True

def location_get(c, sender, path, iface, name):
    return {"Latitude": GLib.Variant("d", 48.856613), "Longitude": GLib.Variant("d", 2.352222), "Accuracy": GLib.Variant("d", 20000.0),
            "Description": GLib.Variant("s", "Paris, France")}.get(name)

conn = Gio.DBusConnection.new_for_address_sync(address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
conn.register_object("/org/freedesktop/GeoClue2/Manager", Gio.DBusNodeInfo.new_for_xml(MANAGER_XML).interfaces[0], on_manager, None, None)
conn.register_object(CLIENT, Gio.DBusNodeInfo.new_for_xml(CLIENT_XML).interfaces[0], on_client, client_get, client_set)
conn.register_object(LOCATION, Gio.DBusNodeInfo.new_for_xml(LOCATION_XML).interfaces[0], None, location_get, None)
Gio.bus_own_name_on_connection(conn, "org.freedesktop.GeoClue2", Gio.BusNameOwnerFlags.NONE, None, None)
print("ready", flush=True)
GLib.MainLoop().run()
