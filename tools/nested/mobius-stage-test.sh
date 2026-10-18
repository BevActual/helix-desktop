#!/bin/bash
# The stage: a reply's artifact block opens as a document beside the chat, the person edits it, versions are kept, and the open document goes
# to the model with the next message as a chip.
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
STAGE=$(ls "$SCRATCH"/data/helix/mobius/stage/*.json 2>/dev/null | head -1)
click_id input; wtype "Draft a plan"; wtype -k Return; idle
wait_id artifact-card && echo "ok: the reply's document is a card" || fail "no artifact card"
click_id artifact-open
wait_id stage-text && echo "ok: it opened on the stage" || fail "no stage"
wait_id stage-chip && echo "ok: the document is attached as a visible chip" || fail "no chip"
shot stage; sleep 1; shot stage
echo "--- editing keeps a version"
click_id stage-text; wtype " EDITED"; sleep 3
STAGE=$(ls "$SCRATCH"/data/helix/mobius/stage/*.json | head -1)
python3 - "$STAGE" <<'PY' && echo "ok: the person's edit is a new version" || fail "edit not kept"
import json, sys
d = json.load(open(sys.argv[1]))["documents"][0]
assert d["title"] == "Plan" and [v["by"] for v in d["versions"]] == ["model", "person"], d
assert "EDITED" in d["versions"][-1]["body"]
PY
echo "--- the open document goes with the next message"
click_id input; wtype "Make it shorter"; wtype -k Return; idle
python3 - "$SCRATCH/data/helix/mobius/chats" <<'PY' && echo "ok: the model was sent the document as edited" || fail "document not sent"
import glob, json, sys
c = json.load(open(glob.glob(sys.argv[1] + "/*.json")[0]))
user = [m["text"] for m in c["messages"] if m["role"] == "user"][-1]
assert "EDITED" in user and "Make it shorter" in user, user
PY
[ "$FAILED" = 0 ] && echo "PASS: mobius stage" || exit 1
