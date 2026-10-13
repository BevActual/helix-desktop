#!/usr/bin/env python3
"""A stand-in agent for nested tests. Usage:
  agent-probe.py grant <tiers...>   Hello + RequestAccess; prints COMPLETED ok=<bool> result=<json>
  agent-probe.py act <tool> [arg...] Hello + Action; prints COMPLETED ok=<bool> result=<json>
Same executable path every time, so the service sees the same client and its grants persist between calls.
Exit 0 whether or not the operation succeeded (the test reads the printed result); 2 on timeout."""
import json
import os
import sys
import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

NAME, PATH = "org.helix.Desktop", "/org/helix/Desktop"
AGENT = "org.helix.Desktop.AgentSurface1"
NO_AUTO = Gio.DBusCallFlags.NO_AUTO_START

conn = Gio.DBusConnection.new_for_address_sync(
    os.environ["DBUS_SESSION_BUS_ADDRESS"],
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
    None, None)
completed = []
conn.signal_subscribe(NAME, AGENT, "Completed", PATH, None, Gio.DBusSignalFlags.NONE,
                      lambda *a: completed.append(a[5].unpack()))


def call(method, params, reply):
    return conn.call_sync(NAME, PATH, AGENT, method, params, GLib.VariantType(reply), NO_AUTO, 5000, None).unpack()


call("Hello", GLib.Variant("(ss)", ("nested activity probe", "1")), "(a{sv})")
verb, rest = sys.argv[1], sys.argv[2:]
if verb == "grant":
    (operation,) = call("RequestAccess", GLib.Variant("(as)", (rest,)), "(t)")
elif verb == "act":
    options = {"argv": GLib.Variant("as", rest[1:]), "literal": GLib.Variant("as", []), "reason": GLib.Variant("s", "nested test")}
    (operation,) = call("Action", GLib.Variant("(sa{sv})", (rest[0], options)), "(t)")
else:
    sys.exit("unknown verb")
print(f"REQUESTED {operation}", flush=True)
context = GLib.MainContext.default()
end = time.time() + 30
while time.time() < end:
    for op, ok, text in completed:
        if op == operation:
            print(f"COMPLETED ok={ok} result={json.dumps(json.loads(text))}", flush=True)
            sys.exit(0)
    while context.iteration(False):
        pass
    time.sleep(0.05)
print("TIMEOUT", file=sys.stderr)
sys.exit(2)
