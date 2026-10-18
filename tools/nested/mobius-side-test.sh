#!/bin/bash
# Artifacts (every document the chats made, in the sidebar) and side chats (/side: a temporary chat that reads the main one).
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-side-test.sh tools/nested/run.sh mobius-side tools/nested/mobius-nested.sh <wide window> "HELIX_FAKE_REPLY=..."
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
click_id input; wtype "Draft a plan"; wtype -k Return; idle
echo "--- Artifacts lists the document and opens it"
click_id mode-artifacts
wait_id artifact-row-0 && echo "ok: the document is listed" || fail "no artifact row"
shot artifacts; sleep 1; shot artifacts
click_id artifact-row-0
wait_id stage-text && echo "ok: it opened on the stage" || fail "no stage"
click_id mode-chat; sleep 1
echo "--- a side chat reads the main chat"
click_id input; wtype "/side"; wtype -k Return; sleep 2
wait_id side-chip && echo "ok: a side chat started" || fail "no side chip"
wait_id attach-chip-0 && echo "ok: the main chat is attached for it to read" || fail "no attachment"
shot side; sleep 1; shot side
click_id side-chip; sleep 2
has_id side-chip && fail "the chip stayed" || echo "ok: back in the main chat"
wait_id msg-1 && echo "ok: the main chat is untouched" || fail "main chat missing"
[ "$FAILED" = 0 ] && echo "PASS: mobius side" || exit 1
