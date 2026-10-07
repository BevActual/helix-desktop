"""Where to click a named control, from the layout the page reported (HELIX_UI_LAYOUT_LOG).
  ui_pos.py <layout.json> <control name> [fraction along its width]  ->  prints "fx fy" (fractions of the screen) for wl-pointer.
Waits up to 8 s for the control to exist and for its position to stop changing, so a click never lands on a stale layout.
Exits 1 when the control never appears."""
import json, sys, time
path, name = sys.argv[1], sys.argv[2]
along = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
def read():
    try:
        data = json.load(open(path))
        box = data["elements"].get(name)
        return (data["width"], data["height"], box) if box else None
    except Exception:
        return None
end, last = time.time() + 8, None
while time.time() < end:
    now = read()
    if now and now == last:
        w, h, box = now
        print(f"{(box['x'] + box['w'] * along) / w:.4f} {(box['y'] + box['h'] / 2) / h:.4f}")
        sys.exit(0)
    last = now
    time.sleep(0.3)
sys.exit(1)
