#!/usr/bin/env python3
"""A stand-in media player (MPRIS) on the session bus. Records PlayPause calls in $MPRIS_LOG; reads commands appended to $MPRIS_CMD:
"status <Playing|Paused|Stopped>", "title <text>", "quit"."""
import os

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

LOG, CMD = os.environ["MPRIS_LOG"], os.environ["MPRIS_CMD"]
state = {"status": "Playing", "title": "Test Song", "artist": "Test Artist"}
XML = """<node>
<interface name="org.mpris.MediaPlayer2"><property name="Identity" type="s" access="read"/><method name="Quit"/><method name="Raise"/>
<property name="CanQuit" type="b" access="read"/><property name="CanRaise" type="b" access="read"/><property name="HasTrackList" type="b" access="read"/></interface>
<interface name="org.mpris.MediaPlayer2.Player"><method name="PlayPause"/><method name="Play"/><method name="Pause"/><method name="Next"/><method name="Previous"/>
<property name="PlaybackStatus" type="s" access="read"/><property name="Metadata" type="a{sv}" access="read"/>
<property name="CanPlay" type="b" access="read"/><property name="CanPause" type="b" access="read"/><property name="CanControl" type="b" access="read"/>
<property name="CanGoNext" type="b" access="read"/><property name="CanGoPrevious" type="b" access="read"/><property name="CanSeek" type="b" access="read"/>
<property name="Position" type="x" access="read"/><property name="Rate" type="d" access="read"/><property name="Volume" type="d" access="read"/></interface></node>"""


def note(text):
    with open(LOG, "a") as f:
        f.write(text + "\n")


def metadata():
    return {"mpris:trackid": GLib.Variant("o", "/track/1"), "xesam:title": GLib.Variant("s", state["title"]),
            "xesam:artist": GLib.Variant("as", [state["artist"]])}


def on_call(conn, sender, path, iface, method, params, invocation):
    if method == "PlayPause":
        state["status"] = "Paused" if state["status"] == "Playing" else "Playing"
        changed()
    note(method)
    invocation.return_value(None)


def on_get(conn, sender, path, iface, name):
    if name == "PlaybackStatus":
        return GLib.Variant("s", state["status"])
    if name == "Metadata":
        return GLib.Variant("a{sv}", metadata())
    if name in ("Identity",):
        return GLib.Variant("s", "Fake Player")
    if name in ("Position",):
        return GLib.Variant("x", 0)
    if name in ("Rate", "Volume"):
        return GLib.Variant("d", 1.0)
    return GLib.Variant("b", name.startswith("Can") and name != "CanQuit")


def changed():
    conn.emit_signal(None, "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "PropertiesChanged",
                     GLib.Variant("(sa{sv}as)", ("org.mpris.MediaPlayer2.Player", {"PlaybackStatus": GLib.Variant("s", state["status"]),
                                                  "Metadata": GLib.Variant("a{sv}", metadata())}, [])))


conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
node = Gio.DBusNodeInfo.new_for_xml(XML)
for iface in node.interfaces:
    conn.register_object("/org/mpris/MediaPlayer2", iface, on_call, on_get, None)
Gio.bus_own_name_on_connection(conn, "org.mpris.MediaPlayer2.fake", Gio.BusNameOwnerFlags.NONE, None, None)
offset = [0]


def poll():
    try:
        with open(CMD) as f:
            f.seek(offset[0])
            lines = f.read().splitlines()
            offset[0] = f.tell()
    except FileNotFoundError:
        return True
    for line in lines:
        if line.startswith("status "):
            state["status"] = line[7:].strip(); changed()
        elif line.startswith("title "):
            state["title"] = line[6:]; changed()
        elif line == "quit":
            loop.quit()
    return True


GLib.timeout_add(150, poll)
note("ready")
loop = GLib.MainLoop()
loop.run()
