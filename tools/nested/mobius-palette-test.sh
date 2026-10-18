#!/bin/bash
# The command palette (Ctrl+K): finds a chat, runs a command, and can start a chat with the words typed.
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-palette-test.sh tools/nested/run.sh mobius-palette tools/nested/mobius-nested.sh <wide window>
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
chat_rows() { python3 -c "import json;print(sum(1 for k in json.load(open('$SCRATCH/state/layout.json'))['elements'] if k.startswith('palette-row-chat:')))"; }
click_id input; wtype "Tell me about cron"; wtype -k Return; sleep 4
echo "--- Ctrl+K opens it"
click_id input; wtype -M ctrl -k k -m ctrl
wait_id palette-input && echo "ok: the palette opened" || fail "no palette"
wait_id palette-row-new && echo "ok: commands are listed" || fail "no commands"
shot palette; sleep 1; shot palette
echo "--- typing narrows to a command, Enter runs it"
wtype "memo"; sleep 1
wait_id palette-row-memory && echo "ok: the Memory command matched" || fail "no memory row"
wtype -k Return
wait_id memory-panel && echo "ok: Enter ran it" || fail "memory did not open"
click_id memory-done; sleep 1
echo "--- it finds the chat by what was said in it"
click_id input; wtype -M ctrl -k k -m ctrl; wait_id palette-input; wtype "cron"; sleep 2
[ "$(chat_rows)" -ge 1 ] && echo "ok: the chat is offered" || fail "the chat was not found"
wait_id palette-row-ask && echo "ok: and a way to ask it as a new chat" || fail "no ask row"
wtype -k Escape; sleep 1
has_id palette-input && fail "Escape did not close it" || echo "ok: Escape closed it"
[ "$FAILED" = 0 ] && echo "PASS: mobius palette" || exit 1
