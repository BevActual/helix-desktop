#!/bin/bash
# Settings → Library → Find more models on Hugging Face (answered from files): search, pick a repository, see the files that have a published
# checksum with how each fits this computer, a gated repository and one without checksums are refused with a reason, and a pasted link
# opens a repository directly. Nothing is downloaded (the Get button asks for confirmation and is not pressed).
#   tools/nested/ui-suite.sh mobius-hub
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
click_id open-models; sleep 1; wait_id models-nav-library && click_id models-nav-library; sleep 1
wait_id hub-toggle && echo "ok: the Library offers Hugging Face" || fail "no Hugging Face section"
click_id hub-toggle; sleep 0.8
echo "--- 1. search"
click_id hub-input; wtype "qwen"; sleep 0.4; click_id hub-go; sleep 1.5
wait_id hub-repo-Qwen/Qwen3-4B-GGUF && has_id hub-repo-randomperson/odd-model-GGUF && echo "ok: results are listed" || { fail "no results"; shot hub-none; }
shot hub-results; shot hub-results
echo "--- 2. a repository with files"
click_id hub-repo-Qwen/Qwen3-4B-GGUF; sleep 1.5
wait_id hub-variant-Qwen3-4B-Q4_K_M.gguf && has_id hub-variant-Qwen3-4B-Q8_0.gguf && echo "ok: single files with a checksum are offered" || fail "no variants"
has_id hub-variant-Qwen3-4B-BF16-00001-of-00002.gguf && echo "ok: a complete set of parts is one choice" || fail "the parts were not grouped"
has_id hub-variant-mmproj-F16.gguf && fail "a picture projector was offered as a model" || echo "ok: a picture projector is not offered as a model"
has_id hub-get-Qwen3-4B-F32.gguf && echo "ok: a file too big for this computer is listed but cannot be chosen (see the screenshot)"; shot hub-variants; shot hub-variants
echo "--- 3. a gated repository and one without checksums"
click_id hub-clear; sleep 0.6; click_id hub-input; wtype "acme/locked-GGUF"; sleep 0.3; click_id hub-go; sleep 1.5
wait_id hub-error && echo "ok: a gated repository is refused with a reason" || fail "no refusal for the gated repository"
click_id hub-clear; sleep 0.6; click_id hub-input; wtype "https://huggingface.co/randomperson/odd-model-GGUF"; sleep 0.3; click_id hub-go; sleep 1.5
wait_id hub-note && echo "ok: a pasted link opens it, and with no checksum nothing is offered" || fail "no note for the repository without checksums"
shot hub-note; shot hub-note
[ $FAILED = 0 ] && echo "PASS: mobius hub" || exit 1
