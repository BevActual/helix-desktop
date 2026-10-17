#!/bin/bash
# A message that reads like a desktop command gets a one-click offer to use Act; an ordinary question does not. Needs the embedding model
# (HELIX_EMBED_DIR, a bge-small-en-v1.5 folder) and skips without it.
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-hint-test.sh tools/nested/run.sh mobius-hint tools/nested/mobius-nested.sh <wide window> HELIX_EMBED_DIR=<dir>
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
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
[ -f "${HELIX_EMBED_DIR:-/nonexistent}/model.safetensors" ] || { echo "SKIP: no embedding model (set HELIX_EMBED_DIR)"; exit 0; }
echo "--- 1. a command offers Act"
click_id input; wtype "Turn the volume down a bit"
wait_id act-hint 24 && echo "ok: the offer appeared" || fail "no offer for a desktop command"
waited() { for _ in $(seq 1 120); do ! has_id stop && return 0; sleep 0.25; done; return 1; }
echo "--- 2. sending it takes the offer away"
wtype -k Return; sleep 2
gone_id act-hint && echo "ok: gone once sent" || fail "the offer stayed after sending"
waited; sleep 1
echo "--- 3. an ordinary question gets no offer"
click_id input; wtype "What is the capital of Peru?"; sleep 4
has_id act-hint && fail "an offer for a plain question" || echo "ok: no offer"
wtype -k Return; sleep 2; waited; sleep 1
echo "--- 4. taking the offer switches to Act"
click_id input; wtype "Please make the screen a bit brighter"
wait_id act-hint 24 && echo "ok: offered again" || fail "no offer"
click_id act-hint
gone_id act-hint && echo "ok: taken" || fail "the offer stayed after it was taken"
[ "$FAILED" = 0 ] && echo "PASS: mobius hint" || exit 1
