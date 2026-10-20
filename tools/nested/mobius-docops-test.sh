#!/bin/bash
# Write operations: a reply carries a document and a docops block; the block is a card of edits in words, and nothing changes until Apply,
# which makes a new version by the model with the edits done.
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-stage-test.sh tools/nested/run.sh mobius-stage tools/nested/mobius-nested.sh <wide window> "HELIX_FAKE_REPLY=..."
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
click_id input; wtype "Draft a plan and tighten it"; wtype -k Return; idle
wait_id artifact-card && echo "ok: the document is a card" || fail "no artifact card"
wait_id docedits-card && echo "ok: the edits are a card" || fail "no edits card"
click_id artifact-open; wait_id stage-text || fail "no stage"
STAGE=$(ls "$SCRATCH"/data/helix/mobius/stage/*.json | head -1)
versions() { python3 -c "import json;d=json.load(open('$STAGE'))['documents'][0];print(len(d['versions']),d['versions'][-1]['by'])"; }
[ "$(versions)" = "1 model" ] && echo "ok: nothing changed before Apply" || fail "changed early: $(versions)"
click_id docedits-apply; wait_id pending-accept 20 && echo "ok: the edits open as a review on the page" || fail "no review"
[ "$(versions)" = "1 model" ] && echo "ok: still nothing changed" || fail "changed before Accept: $(versions)"
click_id pending-edit-1; sleep 1.5   # leave the append out
shot docops; sleep 1.5; shot docops
click_id pending-accept; sleep 2
python3 - "$STAGE" <<'PY' && echo "ok: Accept applied only the edit that was left on" || fail "edits not applied"
import json, sys
d = json.load(open(sys.argv[1]))["documents"][0]
assert [v["by"] for v in d["versions"]] == ["model", "model"], d
b = d["versions"][-1]["body"]
assert "collect the evidence" in b and "gather the facts" not in b and "Step three" not in b, b
assert "gather the facts" in d["versions"][0]["body"], "the earlier version is kept"
PY
[ "$FAILED" = 0 ] && echo "PASS: mobius docops" || exit 1
