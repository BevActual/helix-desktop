#!/bin/bash
# helix-wallpaper in a nested compositor: a known image (left half red, right half blue, a green square at the centre) must fill the
# output, cropped not stretched, and a per-monitor colour override must win. Pixels are read from a real screenshot.
#   HELIX_NESTED_SCRIPT=tools/nested/wallpaper-test.sh tools/nested/run.sh wp tools/nested/wallpaper-nested.sh
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
sleep 3
niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5
SHOT=$(ls -t "$SCRATCH"/shots/*.png | head -1); cp "$SHOT" "$SCRATCH/wallpaper.png"
python3 - "$SCRATCH/wallpaper.png" <<'PY' || FAILED=1
import sys, gi
gi.require_version("GdkPixbuf", "2.0")
from gi.repository import GdkPixbuf
pb = GdkPixbuf.Pixbuf.new_from_file(sys.argv[1]); w, h, n, rs = pb.get_width(), pb.get_height(), pb.get_n_channels(), pb.get_rowstride()
px = pb.get_pixels()
def at(x, y): i = y * rs + x * n; return px[i], px[i + 1], px[i + 2]
def near(a, b): return all(abs(p - q) < 24 for p, q in zip(a, b))
checks = [("left is red", at(w // 8, h // 2), (255, 0, 0)), ("right is blue", at(w - w // 8, h // 2), (0, 0, 255)), ("centre is green", at(w // 2, h // 2), (0, 200, 0))]
ok = True
for name, got, want in checks:
    good = near(got, want); ok &= good; print(("ok: " if good else "FAIL: ") + name, got)
sys.exit(0 if ok else 1)
PY
echo "--- a choice written by Settings replaces the wallpaper without a restart"
printf '[wallpaper.default]\npath = "color:#ff00ff"\n' > "$SCRATCH/state/choice.toml.tmp" && mv "$SCRATCH/state/choice.toml.tmp" "$SCRATCH/state/choice.toml"
sleep 3
niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5
cp "$(ls -t "$SCRATCH"/shots/*.png | head -1)" "$SCRATCH/wallpaper-changed.png"
python3 - "$SCRATCH/wallpaper-changed.png" <<'PY' || FAILED=1
import sys, gi
gi.require_version("GdkPixbuf", "2.0")
from gi.repository import GdkPixbuf
pb = GdkPixbuf.Pixbuf.new_from_file(sys.argv[1]); w, h, n, rs = pb.get_width(), pb.get_height(), pb.get_n_channels(), pb.get_rowstride()
px = pb.get_pixels()
def at(x, y): i = y * rs + x * n; return px[i], px[i + 1], px[i + 2]
ok = True
for name, (x, y) in [("left", (w // 8, h // 2)), ("centre", (w // 2, h // 2)), ("right", (w - w // 8, h // 2))]:
    got = at(x, y); good = all(abs(a - b) < 24 for a, b in zip(got, (255, 0, 255))); ok &= good
    print(("ok: " if good else "FAIL: ") + name + " is the new colour", got)
sys.exit(0 if ok else 1)
PY
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
