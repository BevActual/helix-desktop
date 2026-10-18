#!/bin/bash
# Ask on the selection: select text in a document, a bar offers Shorter, Improve and so on, the model's answer is a change shown old and new,
# and nothing changes until it is accepted (Enter) or it is rejected (Esc).
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-inline-test.sh tools/nested/run.sh mobius-inline tools/nested/mobius-nested.sh <wide window> "HELIX_FAKE_REPLY=..."
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
dclick_id() { local pos; pos=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1" "${2:-0.3}"); "$PT" $pos dclick; sleep 0.8; }
drag_id() { local a b; a=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1" "${2:-0.1}"); b=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1" "${3:-0.5}"); "$PT" $a drag $b; sleep 1; }
idle() { sleep 2; for _ in $(seq 1 120); do ! has_id stop && break; sleep 0.5; done; sleep 1.5; }
click_id input; wtype "Draft a plan"; wtype -k Return; idle
for _ in 1 2 3 4 5 6; do click_id artifact-open; has_id stage-text && break; sleep 3; done; wait_id stage-text || { shot nostage; sleep 1; shot nostage; fail "no stage"; }
echo "--- selecting text shows the bar"
drag_id stage-text 0.08 0.55; shot sel; sleep 1; shot sel
wait_id stage-selbar && echo "ok: the selection bar appeared" || fail "no selection bar"
wait_id sel-shorter && wait_id sel-custom && echo "ok: it offers the named edits and a free instruction" || fail "bar incomplete"
echo "--- an edit is a proposal, and rejecting changes nothing"
click_id sel-shorter
wait_id stage-proposal 60 && echo "ok: the answer is a proposed change" || fail "no proposal"
shot inline; sleep 1; shot inline
click_id proposal-reject; sleep 1
has_id stage-proposal && fail "the proposal stayed" || echo "ok: rejected"
python3 - "$SCRATCH/data/helix/mobius/stage" <<'PY' && echo "ok: rejecting kept the document as it was" || fail "document changed"
import glob, json, sys
d = json.load(open(glob.glob(sys.argv[1] + "/*.json")[0]))["documents"][0]
assert len(d["versions"]) == 1, d["versions"]
PY
echo "--- accepting writes it as the person's version"
drag_id stage-text 0.08 0.55; shot sel; sleep 1; shot sel
click_id sel-improve; wait_id proposal-accept 60 || fail "no second proposal"
click_id proposal-accept; sleep 3
python3 - "$SCRATCH/data/helix/mobius/stage" <<'PY' && echo "ok: the accepted change is a new version by the person" || fail "accept not saved"
import glob, json, sys
d = json.load(open(glob.glob(sys.argv[1] + "/*.json")[0]))["documents"][0]
assert [v["by"] for v in d["versions"]] == ["model", "person"], [v["by"] for v in d["versions"]]
assert d["versions"][1]["body"] != d["versions"][0]["body"]
PY
[ "$FAILED" = 0 ] && echo "PASS: mobius inline" || exit 1
