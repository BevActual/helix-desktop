#!/bin/bash
# Mobius experiments: persona chip and menu, style overlay (reaches the system prompt), project instructions, persona starters.
#   tools/nested/ui-suite.sh mobius-lab
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { rm -f "$SCRATCH"/shots/*.png; niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
LOG=$SCRATCH/program.log
CHATS=$SCRATCH/data/helix/mobius/chats
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
wait_turn() { for _ in $(seq 1 120); do [ "$(grep -c 'MOBIUS turn ended' "$LOG")" -ge "$1" ] && return 0; sleep 0.25; done; return 1; }
for _ in $(seq 1 80); do [ -f "$SCRATCH/state/layout.json" ] && has_id input && break; sleep 0.25; done; sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "mobius" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
echo "--- 1. chips replace the rail"
wait_id meta-more && click_id meta-more && wait_id menu-more-persona && wait_id menu-more-style && echo "ok: persona and style are in the settings menu" || fail "settings entries missing"; wtype -k Escape; sleep 0.5
has_id persona-dot-0 && fail "the persona rail is still shown with the chip on" || echo "ok: the rail is hidden while the chip is on"
shot lab-chips
echo "--- 2. persona menu"
click_id meta-more; click_id menu-more-persona; sleep 0.8
wait_id menu-persona && echo "ok: the persona menu opened" || fail "no persona menu"
wait_id menu-persona-new && echo "ok: it offers New persona" || fail "no new persona entry"
shot lab-persona-menu
click_id menu-persona-new; sleep 1
wait_id persona-icons && wait_id persona-edit-starters && echo "ok: the persona sheet has icon, description and starters" || fail "persona sheet fields missing"
click_id persona-icon-music; wtype -k Tab 2>/dev/null; sleep 0.2
click_id persona-edit-starters; wtype "Plan my week"; sleep 0.3
click_id persona-save; sleep 1.5
echo "--- 3. the persona's starters show on an empty chat"
wait_id suggestion-0 && echo "ok: starters are on the empty chat" || fail "no suggestions"
shot lab-starters
echo "--- 4. style overlay reaches the prompt"
click_id meta-more; click_id menu-more-style; sleep 0.8
wait_id menu-style-concise && echo "ok: the style menu lists styles" || fail "no style menu"
click_id menu-style-concise; sleep 1
click_id input; wtype "How does cron work?"; wtype -k Return
wait_turn 1 && echo "ok: the turn ran" || fail "no turn"
grep -a "MOBIUS system-prompt" "$LOG" | tail -1 | grep -q "Style for this conversation" && grep -a "MOBIUS system-prompt" "$LOG" | tail -1 | grep -q "briefly" && echo "ok: the concise style is in the system prompt" || { fail "style not in the system prompt"; grep -a "MOBIUS system-prompt" "$LOG" | tail -1 | cut -c1-300; }
F=$(ls -t "$CHATS"/*.json 2>/dev/null | head -1)
grep -q '"style":"concise"' "$F" && echo "ok: the chat remembers its style" || fail "style not saved with the chat"
echo "--- 5. project instructions reach the prompt"
click_id project-chat; sleep 0.8
wait_id project-instructions && echo "ok: the project sheet has instructions" || fail "no project instructions field"
click_id project-name; wtype "Homelab"; click_id project-instructions; wtype "Prefer systemd units."; sleep 0.3
click_id project-name; wtype -k Return; sleep 1.5
shot lab-project
click_id input; wtype "And what about timers?"; wtype -k Return
wait_turn 2 && echo "ok: the second turn ran" || fail "no second turn"
LAST=$(grep -a "MOBIUS system-prompt" "$LOG" | tail -1)
# A slow nested compositor can auto-repeat a held key, doubling letters; the match tolerates repeats.
echo "$LAST" | grep -q "Instructions for this project" && echo "$LAST" | grep -qE "P+r+e+f+e+r+ +s+y+s+t+e+m+d+" && echo "ok: the project's instructions are in the system prompt" || { fail "project instructions not in the prompt"; echo "$LAST" | cut -c1-400; }
echo "$LAST" | grep -q "Style for this conversation" && echo "ok: the style is still applied" || fail "style lost"
echo "--- 5b. notes and usage"
click_id input; wtype "/note export a chat as markdown"; wtype -k Return; sleep 1.5
W=$(find "$SCRATCH/state" -name wishlist.md 2>/dev/null | head -1)
[ -n "$W" ] && grep -qE "e+x+p+o+r+t+" "$W" && echo "ok: /note wrote a line to the wishlist" || fail "no wishlist line"
U=$(find "$SCRATCH/state" -name usage.jsonl 2>/dev/null | head -1)
[ -n "$U" ] && grep -q '"kind":"send"' "$U" && grep -q '"kind":"turn"' "$U" && echo "ok: usage events are logged (no message text)" || fail "no usage log"
grep -q "cron" "$U" && fail "message text leaked into the usage log" || echo "ok: the usage log holds no message text"
echo "--- 6. experiments switch the layers off"
click_id open-models; sleep 1.5
wait_id models-nav-advanced && click_id models-nav-advanced; sleep 0.8
wait_id experiment-style_overlays && echo "ok: the experiments are listed" || fail "no experiments panel"
click_id experiment-style_overlays; sleep 0.8
click_id models-save; sleep 1.5
click_id meta-more; sleep 0.5; has_id menu-more-style && fail "the style entry stayed after turning the experiment off" || echo "ok: turning style overlays off removes the chip"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
