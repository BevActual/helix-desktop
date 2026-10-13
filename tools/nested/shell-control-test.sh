#!/bin/bash
# HELIX_NESTED_SCRIPT: org.helix.Shell1 (what a key binding calls), against the Tauri shell.
#   HELIX_NESTED_SCRIPT=tools/nested/shell-control-test.sh tools/nested/run.sh ctl tools/nested/tauri-shell.sh
SCRATCH=$1; LOG=$SCRATCH/program.log
call() { busctl --user call org.helix.Shell /org/helix/Shell org.helix.Shell1 "$@" 2>&1; }
wait_for() { for _ in $(seq 1 "${3:-40}"); do [ "$(grep -c "$1" "$LOG" 2>/dev/null)" -ge "$2" ] && return 0; sleep 0.25; done; echo "TIMEOUT waiting for $2 x $1"; return 1; }
status() { call GetStatus | grep -o '"[a-z-]*"' | tail -1; }
ok() { echo "ok: $1"; }; bad() { echo "FAIL: $1"; }
wait_for "CONTROL serving" 1 60 || exit 1; wait_for "LIVE Sound" 1 60
[ "$(status)" = '""' ] && ok "nothing is open at the start" || bad "start status $(status)"
call TogglePanel s launcher >/dev/null; wait_for "PANEL open" 1 && sleep 0.5; [ "$(status)" = '"launcher"' ] && ok "TogglePanel opened the launcher" || bad "toggle open: $(status)"
call TogglePanel s launcher >/dev/null; wait_for "PANEL closed" 1 && sleep 0.5; [ "$(status)" = '""' ] && ok "TogglePanel again closed it" || bad "toggle close: $(status)"
call OpenPanel s assistant >/dev/null; wait_for "PANEL open" 2; sleep 0.5; [ "$(status)" = '"assistant"' ] && ok "OpenPanel assistant" || bad "open assistant: $(status)"
call OpenPanel s control-center >/dev/null; sleep 1; [ "$(status)" = '"control-center"' ] && ok "opening another panel replaces the open one" || bad "replace: $(status)"
OUT=$(call OpenPanel s settings); echo "$OUT" | grep -qi "unknown panel" && ok "an unknown panel is refused with an error" || bad "unknown panel: $OUT"
[ "$(status)" = '"control-center"' ] && ok "a refused id changed nothing" || bad "after refusal: $(status)"
OUT=$(call OpenPanel s 'launcher; x'); echo "$OUT" | grep -qi "unknown panel" && ok "a malformed id is refused" || bad "malformed id: $OUT"
call ClosePanel >/dev/null; sleep 0.8; [ "$(status)" = '""' ] && ok "ClosePanel closed it" || bad "close: $(status)"
call ClosePanel >/dev/null && ok "ClosePanel with nothing open is harmless" || bad "close when closed"
echo "panel opens logged: $(grep -c 'PANEL open' "$LOG"), closes: $(grep -c 'PANEL closed' "$LOG")"
