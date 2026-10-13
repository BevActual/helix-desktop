#!/usr/bin/env python3
"""A window with one text field, standing in for whatever the person was typing into. Whatever the field holds is written to $TARGET_FILE
after every change, so a test can read what arrived (typed or pasted)."""
import os
import gi

gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk

# APP_ID: the application id this window announces (what the dock matches windows to applications by).
if os.environ.get("APP_ID"):
    GLib.set_prgname(os.environ["APP_ID"])

path = os.environ["TARGET_FILE"]
window = Gtk.Window(title="insert target")
window.set_default_size(500, 120)
entry = Gtk.Entry()
window.add(entry)
entry.connect("changed", lambda e: open(path, "w").write(e.get_text()))
window.connect("destroy", Gtk.main_quit)
window.show_all()
entry.grab_focus()
open(path, "w").write("")
Gtk.main()
