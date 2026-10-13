"""Where to click a named control, from the layout the page reported (HELIX_UI_LAYOUT_LOG).
  ui_pos.py <layout.json> <control name> [fraction along its width]  ->  prints "fx fy" (fractions of the screen) for wl-pointer.
Waits up to 8 s for the control to exist and for its position to stop changing, so a click never lands on a stale layout.
Exits 1 when the control never appears. With UI_POS_NOWAIT=1 it only checks, once, that the control exists (exit 0) or not (exit 1)."""
import json, os, sys, time
path, name = sys.argv[1], sys.argv[2]
along = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
def read():
    try:
        data = json.load(open(path))
        box = data["elements"].get(name)
        return (data["width"], data["height"], box) if box else None
    except Exception:
        return None
# has_id only asks whether the control is there right now: no waiting for it to appear or to settle (a missing control would cost 8 s).
if os.environ.get("UI_POS_NOWAIT"):
    sys.exit(0 if read() else 1)
end, last = time.time() + 8, None
while time.time() < end:
    now = read()
    if now and now == last:
        w, h, box = now
        # A window that is not a full-screen surface reports positions inside itself: UI_ORIGIN is where its corner is on the
        # screen and UI_SCREEN is the screen size (both in the same logical pixels), as niri says.
        if os.environ.get("UI_ORIGIN") and os.environ.get("UI_SCREEN"):
            ox, oy = (float(v) for v in os.environ["UI_ORIGIN"].split())
            w, h = (float(v) for v in os.environ["UI_SCREEN"].split())
            box = dict(box, x=box["x"] + ox, y=box["y"] + oy)
        print(f"{(box['x'] + box['w'] * along) / w:.4f} {(box['y'] + box['h'] / 2) / h:.4f}")
        sys.exit(0)
    last = now
    time.sleep(0.3)
sys.exit(1)
