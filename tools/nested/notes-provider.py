#!/usr/bin/env python3
"""A pretend third-party app for nested tests: owns org.example.Notes and answers org.helix.ActionProvider1.Invoke for notes.add,
notes.remove and notes.list, keeping notes in a file. notes.add says how it is undone (notes.remove with the same text)."""
import json, os, sys
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

STORE = sys.argv[1]
XML = """<node><interface name="org.helix.ActionProvider1">
  <method name="Invoke"><arg name="action" type="s" direction="in"/><arg name="params" type="s" direction="in"/>
    <arg name="reply" type="s" direction="out"/></method>
</interface></node>"""

def notes():
    try:
        return json.load(open(STORE))
    except Exception:
        return []

def save(items):
    json.dump(items, open(STORE, "w"))

def invoke(action, params):
    p = json.loads(params or "{}")
    items = notes()
    if action == "notes.add":
        items.append(p["text"]); save(items)
        return {"done": f"Added “{p['text']}” to Notes", "undo": {"action": "notes.remove", "params": {"text": p["text"]}}}
    if action == "notes.remove":
        if p["text"] in items:
            items.remove(p["text"]); save(items)
        return {"done": f"Removed “{p['text']}” from Notes"}
    if action == "notes.list":
        return {"notes": items}
    raise ValueError("unknown action")

def call(conn, sender, path, iface, method, params, invocation):
    action, text = params.unpack()
    try:
        invocation.return_value(GLib.Variant("(s)", (json.dumps(invoke(action, text)),)))
    except Exception as error:
        invocation.return_dbus_error("org.example.Notes.Error", str(error))

conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
node = Gio.DBusNodeInfo.new_for_xml(XML)
conn.register_object("/org/example/Notes", node.interfaces[0], call, None, None)
Gio.bus_own_name_on_connection(conn, "org.example.Notes", Gio.BusNameOwnerFlags.NONE, None, None)
GLib.MainLoop().run()
