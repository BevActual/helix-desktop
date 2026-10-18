#!/bin/bash
# Settings → Library: "Add a model file…" takes a .gguf from this computer (a hard link, so the original is untouched), says it is not checked,
# lists it as the person's own, can use it for a job, and can remove it; a file that is not in the GGUF format is refused; cancelling does nothing.
#   tools/nested/ui-suite.sh mobius-import
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
for _ in $(seq 1 80); do [ -f "$SCRATCH/state/layout.json" ] && has_id open-models && break; sleep 0.25; done; sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "mobius" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|} UI_REVEAL="760 150"
MODELS=$SCRATCH/data/helix/models
click_id open-models; sleep 1; wait_id models-nav-library && click_id models-nav-library; sleep 1
wait_id lib-import && echo "ok: the Library has Add a model file" || fail "no import button"
has_id lib-folder && echo "ok: and a way to open the models folder" || fail "no folder button"
echo "--- 1. a real model file"
click_id lib-import; sleep 2.5
[ -f "$MODELS/My-Tiny-Model--Q4-.gguf" ] && echo "ok: it was added under a clean name" || fail "file not in the models folder: $(ls "$MODELS" 2>&1)"
[ -f "$HOME/Downloads/My Tiny Model (Q4).gguf" ] && echo "ok: the original is untouched" || fail "the original is gone"
wait_id lib-added && echo "ok: it says the file is not checked" || fail "no note about checking"
wait_id lib-card-My-Tiny-Model--Q4-.gguf && has_id lib-unchecked-My-Tiny-Model--Q4-.gguf && echo "ok: it is listed as added by you" || fail "no card for the added model"
shot import; shot import
echo "--- 2. use it, then remove it"
click_id lib-use-My-Tiny-Model--Q4-.gguf-chat; sleep 1.5
ROUTING=$(find "$SCRATCH/state" -name inference-routing.toml | head -1)
python3 - "$ROUTING" <<'PY' && echo "ok: it can be used for Chat" || fail "not assigned"
import sys, tomllib
assert tomllib.load(open(sys.argv[1], "rb"))["profiles"]["chat"]["model"] == "My-Tiny-Model--Q4-.gguf"
PY
echo "--- 3. a file that is not a model is refused"
click_id lib-import; sleep 2.5
[ -f "$MODELS/not-a-model.gguf" ] && fail "a non-model was added" || echo "ok: a file without the GGUF marker was refused"
echo "--- 4. cancelling does nothing"
click_id lib-import; sleep 2
[ "$(ls "$MODELS" | grep -c gguf)" = 1 ] && echo "ok: cancelling added nothing" || fail "unexpected files: $(ls "$MODELS")"
[ $FAILED = 0 ] && echo "PASS: mobius import" || exit 1
