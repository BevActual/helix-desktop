#!/usr/bin/env python3
"""A stand-in application for notification tests, on the session bus.
  notify-probe.py send <app> <summary> <body> <urgency 0|1|2> <timeout_ms> [key label ...]   prints the id
  notify-probe.py close <id>
  notify-probe.py listen          prints every NotificationClosed / ActionInvoked until killed
"""
import os
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

NAME, PATH, IFACE = "org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications"
conn = Gio.DBusConnection.new_for_address_sync(
    os.environ["DBUS_SESSION_BUS_ADDRESS"],
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
verb, rest = sys.argv[1], sys.argv[2:]
if verb == "send":
    app, summary, body, urgency, timeout = rest[:5]
    hints = {"urgency": GLib.Variant("y", int(urgency))}
    params = GLib.Variant("(susssasa{sv}i)", (app, 0, "", summary, body, rest[5:], hints, int(timeout)))
    print(conn.call_sync(NAME, PATH, IFACE, "Notify", params, GLib.VariantType("(u)"), Gio.DBusCallFlags.NO_AUTO_START, 5000, None).unpack()[0])
elif verb == "close":
    conn.call_sync(NAME, PATH, IFACE, "CloseNotification", GLib.Variant("(u)", (int(rest[0]),)), None, Gio.DBusCallFlags.NO_AUTO_START, 5000, None)
elif verb == "listen":
    def on_signal(_c, _s, _p, _i, name, params):
        print(name, *params.unpack(), flush=True)
    conn.signal_subscribe(NAME, IFACE, None, PATH, None, Gio.DBusSignalFlags.NONE, on_signal)
    GLib.MainLoop().run()
