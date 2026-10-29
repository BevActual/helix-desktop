#!/usr/bin/env python3
"""A scripted systemd-timedated on a private bus, for Settings' Date & time page. It owns org.freedesktop.timedate1 with the properties
Timezone, NTP, CanNTP and NTPSynchronized and the methods ListTimezones, SetTimezone and SetNTP. Every call is appended to
<state dir>/fake-timedate.log (`SetTimezone Europe/Paris interactive=True`).

  fake-timedate.py <state dir> <bus address>
"""
import os, sys
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

state_dir, address = sys.argv[1], sys.argv[2]
log_path = os.path.join(state_dir, "fake-timedate.log")
ZONES = ["Africa/Cairo", "America/Chicago", "America/Los_Angeles", "America/New_York", "Asia/Kolkata", "Asia/Tokyo", "Australia/Sydney", "Europe/London", "Europe/Paris", "UTC"]
state = {"Timezone": "Europe/London", "NTP": True}
connection = None

XML = """<node><interface name="org.freedesktop.timedate1">
<property name="Timezone" type="s" access="read"/><property name="NTP" type="b" access="read"/>
<property name="CanNTP" type="b" access="read"/><property name="NTPSynchronized" type="b" access="read"/>
<method name="ListTimezones"><arg type="as" direction="out"/></method>
<method name="SetTimezone"><arg type="s" direction="in"/><arg type="b" direction="in"/></method>
<method name="SetNTP"><arg type="b" direction="in"/><arg type="b" direction="in"/></method>
</interface></node>"""

def log(line):
    with open(log_path, "a") as handle: handle.write(line + "\n")

def on_call(conn, sender, path, iface, method, params, invocation):
    if method == "ListTimezones":
        invocation.return_value(GLib.Variant("(as)", (ZONES,)))
    elif method == "SetTimezone":
        zone, interactive = params.unpack()
        log(f"SetTimezone {zone} interactive={interactive}")
        if zone not in ZONES:
            invocation.return_dbus_error("org.freedesktop.DBus.Error.InvalidArgs", "Invalid time zone"); return
        state["Timezone"] = zone
        invocation.return_value(None)
    elif method == "SetNTP":
        on, interactive = params.unpack()
        log(f"SetNTP {on} interactive={interactive}")
        state["NTP"] = on
        invocation.return_value(None)

def get_property(conn, sender, path, iface, name):
    if name == "Timezone": return GLib.Variant("s", state["Timezone"])
    if name == "NTP": return GLib.Variant("b", state["NTP"])
    if name == "CanNTP": return GLib.Variant("b", True)
    if name == "NTPSynchronized": return GLib.Variant("b", state["NTP"])
    return None

conn = Gio.DBusConnection.new_for_address_sync(address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
node = Gio.DBusNodeInfo.new_for_xml(XML)
conn.register_object("/org/freedesktop/timedate1", node.interfaces[0], on_call, get_property, None)
Gio.bus_own_name_on_connection(conn, "org.freedesktop.timedate1", Gio.BusNameOwnerFlags.NONE, None, None)
print("ready", flush=True)
GLib.MainLoop().run()
