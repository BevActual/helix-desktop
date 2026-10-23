#!/bin/bash
# Settings > Startup apps: what starts at login is listed with a switch each; turning a system program off writes a file of the person's own with
# Hidden=true (the system's is never touched) and turning it on again removes it; a program can be added from the installed applications.
#   HELIX_NESTED_SCRIPT=tools/nested/settings-startup-test.sh tools/nested/run.sh settings-startup tools/nested/settings-startup-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "settings" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
for _ in $(seq 1 120); do grep -q "SETTINGS bluetooth" "$LOG" && break; sleep 0.25; done; sleep 2
A=$SCRATCH/config/autostart; SYS=$SCRATCH/state/fixture-system-autostart
click_id page-startup; sleep 1.5
wait_id set-start:keyring.desktop 20 && has_id set-start:printer.desktop && has_id set-start:mysync.desktop && echo "ok: the programs that start at login are listed" || fail "rows missing"
shot startup; sleep 1; shot startup
click_id set-start:keyring.desktop; sleep 1.2
grep -q "Hidden=true" "$A/keyring.desktop" && echo "ok: turning the keyring off wrote a Hidden=true file of the person's own" || fail "no override: $(cat "$A/keyring.desktop" 2>&1)"
grep -q "Hidden" "$SYS/keyring.desktop" && fail "the system's file was changed" || echo "ok: the system's file is untouched"
click_id set-start:keyring.desktop; sleep 1.2
[ ! -e "$A/keyring.desktop" ] && echo "ok: turning it on again removed the override" || fail "override left: $(cat "$A/keyring.desktop")"
click_id set-start:printer.desktop; sleep 1.2
[ ! -e "$A/printer.desktop" ] && echo "ok: a system program that was already off turns on by removing its override" || fail "printer override: $(cat "$A/printer.desktop")"
click_id set-start:mysync.desktop; sleep 1.2
grep -q "Hidden=true" "$A/mysync.desktop" && grep -q "Exec=true" "$A/mysync.desktop" && echo "ok: a program the person added is turned off in place, still whole" || fail "mysync: $(cat "$A/mysync.desktop")"
echo "--- adding"
click_id set-add:app; sleep 0.5; wait_id set-add:app-notes.desktop 10 && echo "ok: the list to add from offers installed programs" || fail "no list"
has_id set-add:app-mysync.desktop && fail "a program already on the list is offered again"
click_id set-add:app-notes.desktop; sleep 1.5
grep -q "Name=Notes" "$A/notes.desktop" && echo "ok: Notes was added as a copy of its desktop file" || fail "not added: $(ls "$A")"
wait_id set-start:notes.desktop 10 && echo "ok: and it now has a switch" || fail "no switch for notes"
shot startup-added
[ "$FAILED" = 0 ] && echo "PASS: settings startup" || exit 1
