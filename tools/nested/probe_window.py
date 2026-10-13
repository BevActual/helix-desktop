"""A plain window that logs the pointer and keyboard events it receives, to see what a layer surface lets through.
Prints 'PROBE button x y', 'PROBE key name', 'PROBE focus in|out' lines.  Run with WAYLAND_DISPLAY of the nested session."""
import gi, sys
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, Gdk
w = Gtk.Window(title="probe"); w.set_default_size(900, 600)
w.add_events(Gdk.EventMask.BUTTON_PRESS_MASK | Gdk.EventMask.KEY_PRESS_MASK)
w.connect("button-press-event", lambda _w, e: print(f"PROBE button {e.x:.0f} {e.y:.0f}", flush=True))
w.connect("key-press-event", lambda _w, e: print(f"PROBE key {Gdk.keyval_name(e.keyval)}", flush=True))
w.connect("focus-in-event", lambda *_: print("PROBE focus in", flush=True))
w.connect("focus-out-event", lambda *_: print("PROBE focus out", flush=True))
w.connect("destroy", Gtk.main_quit); w.show_all(); Gtk.main()
