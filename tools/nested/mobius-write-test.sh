#!/bin/bash
# The Write stage as a writing tool: Read view, formatting from the toolbar, outline, history with restore, find and replace, rename.
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-write-test.sh tools/nested/run.sh mobius-write tools/nested/mobius-nested.sh <wide window> "HELIX_FAKE_REPLY=..."
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
drag_id() { local a b; a=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1" "${2:-0.1}"); b=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" "$1" "${3:-0.5}"); "$PT" $a drag $b; sleep 1; }
click_id input; wtype "Draft a plan"; wtype -k Return; idle
for _ in 1 2 3 4 5 6; do click_id artifact-open; has_id stage-text && break; sleep 3; done; wait_id stage-text || fail "no stage"
STAGEJSON() { ls "$SCRATCH"/data/helix/mobius/stage/*.json | head -1; }
latest() { python3 -c "import json;d=json.load(open('$(STAGEJSON)'))['documents'][0];print(d['title'],len(d['versions']),sep='|');print(d['versions'][-1]['body'])"; }
echo "--- the Read view shows the same Markdown, rendered"
click_id stage-mode-read; wait_id stage-read && echo "ok: the Read view opened" || fail "no read view"
shot write-read; sleep 1; shot write-read
click_id stage-mode-write; wait_id stage-text || fail "back to Write failed"
echo "--- formatting from the toolbar"
click_id stage-text
wtype -k Home; sleep 0.2
wtype -M shift-end; sleep 0.2
click_id fmt-bold; sleep 0.8; click_id fmt-heading-2; sleep 3
latest | grep -q '^## .*\*\*' && echo "ok: bold and a heading were applied to the selected line" || fail "formatting: $(latest | head -3)"
echo "--- the outline lists the heading"
has_id outline-0 && fail "the outline should stay out of the way in a narrow stage" || echo "ok: a narrow stage keeps the outline hidden until asked"
click_id stage-outline-toggle; wait_id outline-0 && echo "ok: the outline shows the heading" || fail "no outline"
click_id stage-outline-toggle
echo "--- history keeps every version, and one can be restored"
click_id stage-history-toggle; wait_id version-0 && echo "ok: versions are listed" || fail "no history"
click_id version-0; wait_id stage-diff && wait_id version-restore && echo "ok: a version shows what changed" || fail "no diff"
shot write-history; sleep 1; shot write-history
click_id version-restore; sleep 3
latest | head -1 | grep -q '|[3-9]$' && ! latest | grep -q '^## ' && echo "ok: restoring made a new version with the earlier text" || fail "restore: $(latest | head -2)"
echo "--- find and replace"
click_id stage-more; wait_id stage-find-open && click_id stage-find-open
wait_id find-needle && click_id find-needle && wtype "careful" && click_id find-replacement && wtype "kind" && click_id find-replace-all; sleep 3
latest | grep -q "kind line" && ! latest | grep -q "careful" && echo "ok: replace all changed every match" || fail "find/replace: $(latest | head -4)"
echo "--- renaming the document"
click_id stage-title-input; wtype " II"; wtype -k Return; sleep 2
latest | head -1 | grep -q '^Plan II|' && echo "ok: the title was renamed and the history kept" || fail "rename: $(latest | head -1)"
echo "--- the block menu: / at the start of a line"
click_id stage-text; wtype -k Return; wtype "/hea"; sleep 1
wait_id write-slash && wait_id slash-h1 && echo "ok: typing / offers blocks" || fail "no block menu"
shot write-slash; sleep 1; shot write-slash
wtype -k Return; wtype "Introduction"; sleep 3
latest | grep -q '^# Introduction' && echo "ok: Enter inserted a heading and typing went on" || fail "slash heading: $(latest | tail -3)"
echo "--- continue writing is a proposal, not an edit"
wtype -k Return; wtype "/continue"; sleep 1; wtype -k Return
wait_id stage-proposal 60 && echo "ok: Continue writing proposes text" || fail "no continuation proposal"
click_id proposal-reject; sleep 1
echo "--- focus mode hides the toolbar"
click_id stage-more; click_id stage-focus; sleep 1
has_id fmt-bold && fail "the toolbar stayed in focus mode" || echo "ok: focus mode hid the toolbar"
click_id stage-more; click_id stage-focus; sleep 1
has_id fmt-bold && echo "ok: and brought it back" || fail "toolbar did not return"
[ "$FAILED" = 0 ] && echo "PASS: mobius write" || exit 1
