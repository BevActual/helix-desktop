#!/bin/bash -x
# Mobius against the real model service with a scripted backend: send a message, watch the reply stream in as formatted text,
# stop a reply, start a new chat, reopen the first, delete it. Every control is clicked by name with the real pointer.
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-test.sh tools/nested/run.sh mobius tools/nested/mobius-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
CHATS=$SCRATCH/data/helix/mobius/chats
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
has_id empty && has_id suggestion-0 && echo "ok: an empty chat offers suggestions" || fail "no empty state"
shot chat-empty
echo "--- 1. a message streams in and is saved"
click_id input; wtype "How does cron work?"; wtype -k Return
for _ in $(seq 1 120); do has_id msg-1 && has_id copy-1 && break; sleep 0.25; done
has_id msg-0 && has_id msg-1 && echo "ok: the question and the reply are on screen" || fail "no messages"
sleep 1.5; shot chat-reply
for _ in $(seq 1 80); do grep -q "MOBIUS turn ended" "$LOG" && break; sleep 0.25; done
grep -q "MOBIUS turn ended Finished" "$LOG" && echo "ok: the turn finished" || fail "the turn did not finish: $(tail -3 "$LOG")"
F=$(ls "$CHATS"/*.json 2>/dev/null | head -1)
[ -n "$F" ] && grep -q "scheduler" "$F" && [ "$(stat -c %a "$F")" = 600 ] && echo "ok: saved, owner only" || fail "not saved: $F"
has_id chat-0 && echo "ok: the chat is in the list" || fail "not listed"
echo "--- 1b. search narrows the list"
click_id chat-search; wtype "zzqqxx"; sleep 1.2
has_id chat-0 && fail "a search with no match still lists the chat" || echo "ok: no match, no rows"
click_id chat-search 0.93; sleep 1   # the search field's own clear button
shot search-cleared
has_id chat-0 && echo "ok: clearing the search brings the chat back" || fail "chat missing after clearing the search"
echo "--- 2. a new chat, then back to the first"
click_id new-chat; sleep 1.5; shot chat-new
for _ in $(seq 1 20); do has_id empty && break; sleep 0.25; done
has_id empty && echo "ok: a fresh chat" || { fail "no fresh chat"; shot chat-nofresh; tail -3 "$LOG"; }
click_id chat-0; sleep 1
has_id msg-1 && echo "ok: the first chat reopened with its messages" || fail "did not reopen"
echo "--- 3. stop a reply"
click_id new-chat; sleep 0.8
HELIX=1
click_id input; wtype "Tell me a long story"; wtype -k Return
for _ in $(seq 1 12); do has_id stop && break; sleep 0.2; done
if has_id stop; then click_id stop; echo "ok: stop pressed"; else fail "no stop button"; shot chat-nostop; tail -4 "$LOG" | cut -c1-140; fi
sleep 1.5
grep -c "MOBIUS turn ended" "$LOG" | sed 's/^/turns ended so far: /'
echo "--- 4. delete"
N=$(ls "$CHATS"/*.json 2>/dev/null | wc -l)
pos=$(python3 "$ROOT/tools/nested/ui_pos.py" "$LAYOUT" chat-0 0.5) && "$PT" $pos; sleep 0.6
click_id chat-delete-0; sleep 1
M=$(ls "$CHATS"/*.json 2>/dev/null | wc -l)
[ "$M" -lt "$N" ] && echo "ok: a chat was deleted ($N -> $M)" || fail "nothing deleted ($N -> $M)"
shot chat-after
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
