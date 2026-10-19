#!/bin/bash
# The Image stage: a picture opened from the Image place sits beside the chat; edits are steps on a copy (undo and redo are a position in the list);
# a model's suggested edits are a card that is previewed on the picture and applies only when the person accepts it.
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-image-test.sh tools/nested/run.sh mobius-image tools/nested/mobius-nested.sh <wide window> "HELIX_FAKE_REPLY=..." (with tools/nested/mobius-image-nested.sh)
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
steps() { python3 -c "import json;d=json.load(open('$STAGE'))['docs'][0];print('|'.join(s['label'] for s in d['steps']),d['cursor'],sep=';')" 2>/dev/null; }
wait_id mode-image && click_id mode-image
wait_id img-open && echo "ok: the Image place offers to open a picture" || fail "no Image place"
click_id img-open
wait_id img-picture && echo "ok: the picture opened on the Image stage" || fail "no image stage"
STAGE=$(ls "$SCRATCH"/data/helix/mobius/imagestage/*.json | head -1)
shot image; sleep 1; shot image
echo "--- edits are steps on a copy"
click_id img-grayscale; sleep 2
click_id img-rotate-right; sleep 2
[ "$(steps)" = "Black and white|Rotate 90°;2" ] && echo "ok: two steps recorded" || fail "steps: $(steps)"
click_id img-undo; sleep 1.5
[ "$(steps)" = "Black and white|Rotate 90°;1" ] && echo "ok: undo moved back one step" || fail "after undo: $(steps)"
click_id img-redo; sleep 1.5
[ "$(steps)" = "Black and white|Rotate 90°;2" ] && echo "ok: redo moved forward" || fail "after redo: $(steps)"
python3 - "$SCRATCH/data/helix/mobius/images" "$STAGE" <<'PY' && echo "ok: the original picture is untouched" || fail "original changed"
import json, sys, os, struct
d = json.load(open(sys.argv[2]))["docs"][0]
png = open(os.path.join(sys.argv[1], d["base"]), "rb").read()
w, h = struct.unpack(">II", png[16:24])
assert (w, h) == (40, 30), (w, h)
assert (d["steps"][-1]["width"], d["steps"][-1]["height"]) == (30, 40), d["steps"]
PY
echo "--- a model's suggestion applies only when pressed"
click_id input; wtype "Suggest edits"; wtype -k Return; idle
wait_id edits-card 40 && echo "ok: the suggestion is a card" || { shot image-fail; sleep 1; shot image-fail; fail "no edits card"; }
[ "$(steps)" = "Black and white|Rotate 90°;2" ] && echo "ok: nothing was applied by itself" || fail "applied early: $(steps)"
click_id edits-apply; wait_id pending-img-accept 20 && echo "ok: the suggestion is previewed on the picture" || fail "no preview"
[ "$(steps)" = "Black and white|Rotate 90°;2" ] && echo "ok: the preview changed nothing" || fail "previewing changed it: $(steps)"
shot preview; sleep 1.5; shot preview
click_id pending-img-accept; sleep 3
[ "$(steps)" = "Black and white|Rotate 90°|Brightness +20|Crop to 10×10;4" ] && echo "ok: Apply made the two edits" || fail "after apply: $(steps)"
by=$(python3 -c "import json;d=json.load(open('$STAGE'))['docs'][0];print(','.join(s.get('by','?') for s in d['steps']))" 2>/dev/null)
echo "$by" | grep -q mobius && echo "ok: edits from a suggestion are marked as Mobius's ($by)" || fail "no attribution: $by"
shot image-done; sleep 1; shot image-done
echo "PASS: mobius image"
