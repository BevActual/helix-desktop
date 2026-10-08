#!/usr/bin/env python3
"""A stand-in tray application (StatusNotifierItem) on the session bus. Records every call it receives in $TRAY_LOG.
  tray-app.py            registers, then reads commands appended to $TRAY_CMD: "title <text>", "status <Active|Passive|NeedsAttention>", "quit"
"""
import os
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

LOG = os.environ["TRAY_LOG"]
state = {"Id": "fake-tray", "Title": "Fake Tray", "IconName": "", "Status": "Active", "IconThemePath": ""}
# A 16x16 solid red icon, ARGB32 in network byte order, as the specification sends it.
PIXMAP = [(16, 16, bytes([255, 255, 0, 0]) * 256)]

XML = """<node><interface name="org.kde.StatusNotifierItem">
<property name="Id" type="s" access="read"/><property name="Title" type="s" access="read"/><property name="IconName" type="s" access="read"/>
<property name="IconThemePath" type="s" access="read"/><property name="Status" type="s" access="read"/><property name="IconPixmap" type="a(iiay)" access="read"/>
<method name="Activate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
<method name="ContextMenu"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
<method name="SecondaryActivate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
<signal name="NewTitle"/><signal name="NewStatus"><arg type="s"/></signal>
</interface></node>"""


def note(text):
    with open(LOG, "a") as f:
        f.write(text + "\n")


def on_call(conn, sender, path, iface, method, params, invocation):
    note(f"{method} {params.unpack()[0]},{params.unpack()[1]}")
    invocation.return_value(None)


def on_get(conn, sender, path, iface, name):
    if name == "IconPixmap":
        return GLib.Variant("a(iiay)", PIXMAP)
    return GLib.Variant("s", state[name])


conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
node = Gio.DBusNodeInfo.new_for_xml(XML)
conn.register_object("/StatusNotifierItem", node.interfaces[0], on_call, on_get, None)
Gio.bus_own_name_on_connection(conn, "org.helix.TestTray", Gio.BusNameOwnerFlags.NONE, None, None)
conn.call_sync("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
               GLib.Variant("(s)", ("org.helix.TestTray",)), None, Gio.DBusCallFlags.NONE, 5000, None)
note("registered")


CMD = os.environ["TRAY_CMD"]
offset = [0]


def poll_commands():
    try:
        with open(CMD) as f:
            f.seek(offset[0])
            lines = f.read().splitlines()
            offset[0] = f.tell()
    except FileNotFoundError:
        return True
    for line in lines:
        line = line.strip()
        if line.startswith("title "):
            state["Title"] = line[6:]
            conn.emit_signal(None, "/StatusNotifierItem", "org.kde.StatusNotifierItem", "NewTitle", None)
        elif line.startswith("status "):
            state["Status"] = line[7:]
            conn.emit_signal(None, "/StatusNotifierItem", "org.kde.StatusNotifierItem", "NewStatus", GLib.Variant("(s)", (state["Status"],)))
        elif line == "quit":
            loop.quit()
    return True


GLib.timeout_add(150, poll_commands)  # a test stand-in: polling a command file is the simplest thing that works
loop = GLib.MainLoop()
loop.run()
