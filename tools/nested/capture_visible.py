#!/usr/bin/env python3
"""capture_visible.py <capture.ppm> <layout.json> <wl-capture output> <element>: VISIBLE if the element's rectangle shows content in the capture,
HIDDEN if it is flat, NOCAPTURE if the compositor refused to give a capture at all."""
import json
import sys

ppm, layout, out, name = sys.argv[1:5]
if not open(out).read().startswith("CAPTURED"):
    print("NOCAPTURE")
    sys.exit(0)
data = open(ppm, "rb").read()
head = data.split(b"\n", 3)
w, h = map(int, head[1].split())
pixels = head[3]
d = json.load(open(layout))
sx, sy = w / d["width"], h / d["height"]
e = d["elements"][name]
colours = set()
for y in range(int(e["y"] * sy), int((e["y"] + e["h"]) * sy), 2):
    for x in range(int(e["x"] * sx), int((e["x"] + e["w"]) * sx), 2):
        i = (y * w + x) * 3
        colours.add(pixels[i:i + 3])
# Text on a card has hundreds of colours (anti-aliasing); a blocked-out region is one flat fill.
print("VISIBLE" if len(colours) > 40 else "HIDDEN", len(colours))
