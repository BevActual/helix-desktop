# Source this from a nested test. Needs ROOT, SCRATCH. Gives click_id <name> [fraction] and has_id <name>.
# A test may set LAYOUT first (e.g. the confirmer Overlay writes layout-confirmer.json).
LAYOUT=${LAYOUT:-$SCRATCH/state/layout.json}
PT="$ROOT/tools/nested/wl-pointer/wl-pointer"
# Scrolls the page (Page Down) until a named control is inside the window: a page that has grown past the window's height would otherwise be
# clicked outside it. Does nothing for a control that is already visible.
reveal_id() {
  local _ below
  for _ in 1 2 3 4 5 6 7 8; do
    below=$(UI_POS_NOWAIT=1 python3 - "$LAYOUT" "$1" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1])); b = d["elements"][sys.argv[2]]
    print(1 if b["y"] + b["h"] > d["height"] - 2 else 0)
except Exception:
    print(0)
PY
)
    [ "$below" = 1 ] || return 0
    # Page Down scrolls the area that was last clicked: click an empty spot in the content first (a window reports positions inside itself).
    "$PT" $(python3 - <<'PY'
import json, os
ox, oy = (float(v) for v in os.environ.get("UI_ORIGIN", "0 0").split())
w, h = (float(v) for v in os.environ.get("UI_SCREEN", "1280 720").split())
print(f"{(ox + 520) / w:.4f} {(oy + 60) / h:.4f}")
PY
) click
    wtype -k Next; sleep 1.2   # the page reports its layout every 0.5 s
  done
}
click_id() {
  local pos
  reveal_id "$1"
  pos=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1" "${2:-0.5}") || { echo "NOT FOUND: $1"; return 1; }
  "$PT" ${pos} click; sleep "${CLICK_PAUSE:-0.8}"
}
has_id() { UI_POS_NOWAIT=1 python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1" >/dev/null 2>&1; }
