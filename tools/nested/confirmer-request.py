#!/usr/bin/env python3
"""Request Agent1 access on the nested session bus and print the Completed result.
Usage: confirmer-request.py [--cancel-after SECONDS] <tiers...>
Waits up to 30 s for Completed. Exit 0 on a denied/cancelled grant, 1 on approve, 2 timeout.
With --cancel-after, Cancel is called on the same connection after that many seconds (triggers
ConfirmationEnded for the confirmer).
"""
import argparse
import json
import os
import sys
import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

NAME, PATH = "org.helix.Desktop", "/org/helix/Desktop"
AGENT = "org.helix.Desktop.Agent1"

parser = argparse.ArgumentParser()
parser.add_argument("--cancel-after", type=float, default=None)
parser.add_argument("tiers", nargs="*", default=["observe", "act"])
args = parser.parse_args()
tiers = args.tiers or ["observe", "act"]

address = os.environ["DBUS_SESSION_BUS_ADDRESS"]
conn = Gio.DBusConnection.new_for_address_sync(
    address,
    Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
    | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
    None,
    None,
)
completed = []


def on_signal(_conn, _sender, _path, _iface, name, params):
    if name == "Completed":
        completed.append(params.unpack())


def name_owned():
    return conn.call_sync(
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "NameHasOwner",
        GLib.Variant("(s)", (NAME,)),
        GLib.VariantType("(b)"),
        Gio.DBusCallFlags.NONE,
        2000,
        None,
    ).unpack()[0]


deadline = time.time() + 20
while time.time() < deadline and not name_owned():
    time.sleep(0.1)
if not name_owned():
    print("TIMEOUT waiting for org.helix.Desktop owner", file=sys.stderr)
    sys.exit(3)

conn.signal_subscribe(
    NAME, AGENT, "Completed", PATH, None, Gio.DBusSignalFlags.NONE, on_signal
)
NO_AUTO = Gio.DBusCallFlags.NO_AUTO_START
conn.call_sync(
    NAME,
    PATH,
    AGENT,
    "Hello",
    GLib.Variant("(ss)", ("nested confirmer test", "1")),
    GLib.VariantType("(a{sv})"),
    NO_AUTO,
    5000,
    None,
)
(operation,) = conn.call_sync(
    NAME,
    PATH,
    AGENT,
    "RequestAccess",
    GLib.Variant("(as)", (tiers,)),
    GLib.VariantType("(t)"),
    NO_AUTO,
    5000,
    None,
).unpack()
print(f"REQUESTED {operation}", flush=True)

cancel_at = time.time() + args.cancel_after if args.cancel_after is not None else None
cancelled = False

deadline = time.time() + 30
context = GLib.MainContext.default()
while time.time() < deadline:
    if cancel_at is not None and not cancelled and time.time() >= cancel_at:
        conn.call_sync(
            NAME,
            PATH,
            AGENT,
            "Cancel",
            GLib.Variant("(t)", (operation,)),
            GLib.VariantType("()"),
            NO_AUTO,
            5000,
            None,
        )
        print(f"CANCELLED {operation}", flush=True)
        cancelled = True
    for op, ok, text in list(completed):
        if op == operation:
            result = json.loads(text)
            print(f"COMPLETED ok={ok} result={json.dumps(result)}", flush=True)
            sys.exit(0 if not ok else 1)
    while context.iteration(False):
        pass
    time.sleep(0.05)
print("TIMEOUT waiting for Completed", file=sys.stderr)
sys.exit(2)
