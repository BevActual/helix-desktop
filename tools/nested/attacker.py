#!/usr/bin/env python3
"""A hostile program on the person's session, for nested tests of the trusted surface. Never installed.

  attacker.py cover   a full-screen opaque red Overlay surface (hides what is under it; prints CLICK when it is clicked)
  attacker.py focus   a tiny Overlay surface that asks for exclusive keyboard (prints KEY <name> for every key it receives)

Prints READY once it is on screen. Runs until killed.
"""
import sys

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
gi.require_version("GtkLayerShell", "0.1")
from gi.repository import Gdk, Gtk, GtkLayerShell as LS

mode = sys.argv[1]
window = Gtk.Window()
LS.init_for_window(window)
LS.set_layer(window, LS.Layer.OVERLAY)
LS.set_namespace(window, "attacker-" + mode)
if mode == "cover":
    for edge in (LS.Edge.TOP, LS.Edge.BOTTOM, LS.Edge.LEFT, LS.Edge.RIGHT):
        LS.set_anchor(window, edge, True)
    LS.set_exclusive_zone(window, -1)
    provider = Gtk.CssProvider()
    provider.load_from_data(b"window { background: #ff0000; }")
    Gtk.StyleContext.add_provider_for_screen(Gdk.Screen.get_default(), provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
    window.add_events(Gdk.EventMask.BUTTON_PRESS_MASK)
    window.connect("button-press-event", lambda *_: print("CLICK", flush=True))
else:
    LS.set_keyboard_mode(window, LS.KeyboardMode.EXCLUSIVE)
    window.set_size_request(8, 8)
    window.connect("key-press-event", lambda _w, event: print("KEY", Gdk.keyval_name(event.keyval), flush=True))
window.connect("destroy", Gtk.main_quit)
window.show_all()
window.connect("map-event", lambda *_: print("READY", flush=True))
Gtk.main()
