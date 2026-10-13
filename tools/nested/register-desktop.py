#!/usr/bin/env python3
"""Registers this connection as the desktop (what the shell does), so the service offers the whole settings catalog, and stays registered until killed.
For nested screenshots and tests of the Settings pages that show desktop-only settings."""
import os
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib
conn = Gio.DBusConnection.new_for_address_sync(os.environ["DBUS_SESSION_BUS_ADDRESS"],
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
conn.call_sync("org.helix.Desktop", "/org/helix/Desktop", "org.helix.Desktop.Configuration1", "RegisterDesktop", None, None, 0, 5000, None)
print("REGISTERED", flush=True)
GLib.MainLoop().run()
