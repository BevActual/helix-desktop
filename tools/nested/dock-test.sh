#!/bin/bash
# The dock against two fake applications, with a real pointer: it appears at the bottom edge, a click starts an application, windows are
# matched to it by their app id, a second click focuses, a middle click opens another window, and the menu pins and unpins.
#   HELIX_NESTED_SCRIPT=tools/nested/dock-test.sh tools/nested/run.sh dock tools/nested/dock-nested.sh
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.."  && pwd); LOG=$SCRATCH/program.log; FAILED=0
LAYOUT=$SCRATCH/state/layout.json
PT="$ROOT/tools/nested/wl-pointer/wl-pointer"
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
wait_log() { for _ in $(seq 1 "${2:-60}"); do grep 'DOCK view' "$LOG" 2>/dev/null | tail -1 | grep -Eq "$1" && return 0; sleep 0.25; done; echo "TIMEOUT waiting for: $1 (newest: $(grep 'DOCK view' "$LOG" | tail -1))" >&2; return 1; }
count() { grep -c "$1" "$LOG" 2>/dev/null || true; }
# Positions come from the layout the page reports, so show the dock, then wait for it to settle.
summon() { "$PT" 0.5 0.4; sleep 0.6; "$PT" 0.5 0.999; sleep 1.4; }
pos_of() { python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1" "${2:-0.5}"; }
click_item() { local kind=${2:-click}; local pos; pos=$(pos_of "dock-item-$1" 0.5) || { fail "no dock-item-$1"; return 1; }; "$PT" $pos $kind; sleep 1.2; }

for _ in $(seq 1 120); do grep -q "LIVE Sound" "$LOG" && break; sleep 0.25; done
wait_log 'items=2 running=0' || exit 1
echo "--- 1. the dock appears at the bottom edge, pinned apps in the saved order"
summon
for _ in $(seq 1 20); do has_id dock-item-1 && break; sleep 0.25; done
has_id dock-item-1 || fail "the dock did not appear"
python3 - "$LAYOUT" <<'PY' || FAILED=1
import json, sys
d = json.load(open(sys.argv[1])); e = d["elements"]
w, h = d["width"], d["height"]
item = e.get("dock-item-0")
print("dock-item-0 box:", item)
sys.exit(0 if item and item["y"] > h * 0.85 and abs(item["x"] + item["w"] / 2 - w / 2) < w * 0.25 else 1)
PY
[ $? -eq 0 ] && echo "ok: items sit at the bottom centre" || fail "dock geometry"

echo "--- 2. a click starts the application; its window is matched by app id"
N=$(count "DOCK activate Ok(Launch"); N=$(count "Launch(")
click_item 1   # the second pinned entry is fakeone (faketwo was saved first)
wait_log 'running=1' 40 || fail "the started application was not seen running"
grep -q 'DOCK activate Ok(Launch("fakeone.desktop"))' "$LOG" && echo "ok: Launch(fakeone)" || fail "wrong launch: $(grep 'DOCK activate' "$LOG" | tail -1)"

echo "--- 3. clicking again focuses its window"
summon; click_item 1
grep 'DOCK activate' "$LOG" | tail -1 | grep -q 'Focus(' && echo "ok: focus" || fail "second click was not a focus: $(grep 'DOCK activate' "$LOG" | tail -1)"
grep -q 'DOCK failed' "$LOG" && fail "niri refused: $(grep 'DOCK failed' "$LOG" | tail -1)"

echo "--- 4. a middle click opens another window"
summon; click_item 1 mclick
wait_log 'running=1' 5; sleep 3
niri msg --json windows | python3 -c "import json,sys; w=[x for x in json.load(sys.stdin) if x['app_id']=='fakeone']; print('fakeone windows:', len(w)); sys.exit(0 if len(w)==2 else 1)" && echo "ok: two windows" || fail "no second window"

echo "--- 5. the menu unpins and pins, and the choice is saved"
summon; click_item 0 rclick
for _ in $(seq 1 20); do has_id dock-menu-unpin && break; sleep 0.25; done
has_id dock-menu-unpin || fail "the menu did not open"
click_id dock-menu-unpin
sleep 1
python3 -c "import json; p=json.load(open('$XDG_CONFIG_HOME/helix/dock.json'))['pinned']; print('saved:', p); import sys; sys.exit(0 if p==['fakeone.desktop'] else 1)" && echo "ok: faketwo unpinned and saved" || fail "pins file wrong: $(cat "$SCRATCH/config/helix/dock.json" 2>/dev/null)"

# Tidy: the windows the test started (pids come from niri; stopped by pid)
for pid in $(niri msg --json windows | python3 -c "import json,sys; print(' '.join(str(x['pid']) for x in json.load(sys.stdin) if x['app_id'].startswith('fake')))"); do kill "$pid" 2>/dev/null; done
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
