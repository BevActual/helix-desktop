#!/usr/bin/env python3
"""A window of bright stripes and a fine grid, so blur behind the shell's panels is easy to see in a screenshot."""
import gi

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk

COLOURS = [(1, .23, .19), (1, .62, .04), (.19, .82, .35), (.04, .52, 1), (.75, .35, .95)]


def draw(area, cr):
    width, height = area.get_allocated_width(), area.get_allocated_height()
    for i in range(0, height, 60):
        cr.set_source_rgb(*COLOURS[(i // 60) % len(COLOURS)])
        cr.rectangle(0, i, width, 60)
        cr.fill()
    cr.set_source_rgba(1, 1, 1, .6)
    for x in range(0, width, 42):
        cr.rectangle(x, 0, 4, height)
        cr.fill()
    return False


window = Gtk.Window(title="backdrop")
window.set_decorated(False)
area = Gtk.DrawingArea()
area.connect("draw", draw)
window.add(area)
window.maximize()
window.connect("destroy", Gtk.main_quit)
window.show_all()
Gtk.main()
