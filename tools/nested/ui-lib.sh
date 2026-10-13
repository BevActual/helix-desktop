# Source this from a nested test. Needs ROOT, SCRATCH. Gives click_id <name> [fraction] and has_id <name>.
# A test may set LAYOUT first (e.g. the confirmer Overlay writes layout-confirmer.json).
LAYOUT=${LAYOUT:-$SCRATCH/state/layout.json}
PT="$ROOT/tools/nested/wl-pointer/wl-pointer"
click_id() {
  local pos
  pos=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1" "${2:-0.5}") || { echo "NOT FOUND: $1"; return 1; }
  "$PT" ${pos} click; sleep "${CLICK_PAUSE:-0.8}"
}
has_id() { UI_POS_NOWAIT=1 python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1" >/dev/null 2>&1; }
