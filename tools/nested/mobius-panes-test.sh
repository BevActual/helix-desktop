#!/bin/bash
# The work area is the person's to shape: dragging the divider changes its width, the width is remembered per workspace, double-click resets it,
# and the move button puts it on the other side of the chat.
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
CHATS=$SCRATCH/data/helix/mobius/chats
for _ in $(seq 1 80); do has_id input && break; sleep 0.25; done; sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri", "msg", "--json", "windows"])) if "mobius" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri", "msg", "--json", "outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0, 0]; f = l.get("window_offset_in_tile") or [0, 0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
gone_id() { for _ in $(seq 1 24); do has_id "$1" || return 0; sleep 0.25; done; return 1; }
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
idle() { sleep 2; for _ in $(seq 1 120); do ! has_id stop && break; sleep 0.5; done; sleep 1.5; }
wait_id mode-write && click_id mode-write; wait_id stage-resizer 40 || fail "no divider"; sleep 1
box() { python3 -c "import json;e=json.load(open('$SCRATCH/state/layout.json'))['elements']['$1'];print(int(e['x']),int(e['w']))"; }
W0=$(box stage-host | cut -d' ' -f2)
a=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" stage-resizer 0.5)
python3 - "$a" <<'PY' > /tmp/claude-1000/drag-target
import os, sys
fx, fy = (float(v) for v in sys.argv[1].split())
w, h = (float(v) for v in os.environ["UI_SCREEN"].split())
print(f"{fx - 150 / w:.4f} {fy:.4f}")
PY
"$PT" $a drag $(cat /tmp/claude-1000/drag-target); sleep 1.5
W1=$(box stage-host | cut -d' ' -f2)
[ "$W1" -ge $((W0 + 120)) ] && echo "ok: dragging the divider widened the work area ($W0 to $W1 px)" || fail "drag: $W0 -> $W1"
echo "--- remembered per workspace"
click_id mode-code; sleep 1.5; WC=$(box stage-host | cut -d' ' -f2)
click_id mode-write; sleep 1.5; WW=$(box stage-host | cut -d' ' -f2)
[ "$WW" -ge $((W1 - 4)) ] && [ "$WW" -le $((W1 + 4)) ] && echo "ok: Write kept its width ($WW px); Code has its own ($WC px)" || fail "not remembered: $W1 vs $WW"
echo "--- double-click resets"
a=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" stage-resizer 0.5); "$PT" $a dclick; sleep 1.5
W2=$(box stage-host | cut -d' ' -f2)
[ "$W2" -lt "$W1" ] && echo "ok: double-click went back to the default ($W2 px)" || fail "no reset: $W1 -> $W2"
echo "--- the other side, from the menu"
click_id stage-side-btn; wait_id dock-menu 20 || fail "no dock menu"; click_id dock-left; sleep 1.5
SX=$(box stage-host | cut -d' ' -f1); MX=$(box chat-surface | cut -d' ' -f1)
[ "$SX" -lt "$MX" ] && echo "ok: the work area is left of the chat (x $SX < $MX)" || fail "side: stage $SX chat $MX"
shot movable; sleep 1; shot movable
click_id stage-side-btn; wait_id dock-right 20; click_id dock-right; sleep 1.5
SX=$(box stage-host | cut -d' ' -f1); MX=$(box chat-surface | cut -d' ' -f1)
[ "$SX" -gt "$MX" ] && echo "ok: and back on the right" || fail "did not go back: stage $SX chat $MX"
echo "--- below the chat, by dragging the handle to the bottom edge"
h=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" stage-side-btn 0.5)
python3 - "$h" <<'PY' > /tmp/claude-1000/drag-target2
import os, sys
fx, fy = (float(v) for v in sys.argv[1].split())
w, h = (float(v) for v in os.environ["UI_SCREEN"].split())
ox, oy = (float(v) for v in os.environ["UI_ORIGIN"].split())
print(f"{(ox + 0.5 * 1100) / w:.4f} {(oy + 0.9 * 720) / h:.4f}")
PY
"$PT" $h drag $(cat /tmp/claude-1000/drag-target2); sleep 1.5
vbox() { python3 -c "import json;e=json.load(open('$SCRATCH/state/layout.json'))['elements']['$1'];print(int(e['y']),int(e['h']))"; }
SY=$(vbox stage-host | cut -d' ' -f1); MY=$(vbox chat-surface | cut -d' ' -f1)
[ "$SY" -gt "$MY" ] && echo "ok: dragging the handle to the bottom put the work area below the chat (y $SY > $MY)" || fail "bottom: stage $SY chat $MY"
shot docked-bottom; sleep 1.5; shot docked-bottom
SH0=$(vbox stage-host | cut -d' ' -f2)
a=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" stage-resizer 0.5)
python3 - "$a" <<'PY' > /tmp/claude-1000/drag-target
import os, sys
fx, fy = (float(v) for v in sys.argv[1].split())
w, h = (float(v) for v in os.environ["UI_SCREEN"].split())
print(f"{fx:.4f} {fy - 100 / h:.4f}")
PY
"$PT" $a drag $(cat /tmp/claude-1000/drag-target); sleep 1.5
SH1=$(vbox stage-host | cut -d' ' -f2)
[ "$SH1" -ge $((SH0 + 70)) ] && echo "ok: the divider resizes it vertically too ($SH0 to $SH1 px)" || fail "vertical resize: $SH0 -> $SH1"
click_id stage-side-btn; wait_id dock-right 20; click_id dock-right; sleep 1.5
echo "--- each mode has its own starters and composer"
click_id mode-image; wait_id starter-brighter-more-contrast 20 && echo "ok: Image offers picture starters" || fail "no image starters"
click_id mode-write; wait_id starter-make-it-shorter 20 && echo "ok: Write offers document starters" || fail "no write starters"
has_id starter-brighter-more-contrast && fail "Image starters stayed in Write" || echo "ok: they changed with the mode"
sleep 2; shot starters; sleep 1.5; shot starters
click_id starter-make-it-shorter; idle
python3 - "$CHATS" <<'PY' && echo "ok: a starter sent its prompt" || fail "starter not sent"
import glob, json, sys
c = json.load(open(glob.glob(sys.argv[1] + "/*.json")[0]))
assert any("shorter" in m["text"] for m in c["messages"] if m["role"] == "user"), c["messages"]
PY
[ "$FAILED" = 0 ] && echo "PASS: mobius panes" || exit 1
