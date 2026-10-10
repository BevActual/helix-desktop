#!/bin/bash
# Helix Settings against the real service: it becomes the Agent1 manager, lists who holds which tiers, and a tier switch or a revoke
# changes what the service holds. Controls are clicked by name with the real pointer; the truth is read from the service's own state.
#   HELIX_NESTED_SCRIPT=tools/nested/settings-test.sh tools/nested/run.sh settings tools/nested/settings-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
# (no gaps, so the window sits at the corner of the screen and the pointer maths is exact)
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
LAYOUT=$SCRATCH/state/layout.json; GRANTS=$SCRATCH/state/noctalia/agent/grants.json
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
wait_log() { for _ in $(seq 1 "${2:-60}"); do grep 'SETTINGS agents' "$LOG" | tail -1 | grep -Eq "$1" && return 0; sleep 0.25; done; echo "TIMEOUT waiting for: $1 (newest: $(grep 'SETTINGS agents' "$LOG" | tail -1))" >&2; return 1; }
tiers_of() { python3 -c "import json; print(','.join(sorted(json.load(open('$GRANTS'))['clients'].get('$1', ['<none>']))))"; }
locate_window() {  # where the window is on the screen, from niri, so the pointer lands on the page's controls
  python3 - <<'PY'
import json, subprocess
windows = json.loads(subprocess.check_output(["niri", "msg", "--json", "windows"]))
outputs = json.loads(subprocess.check_output(["niri", "msg", "--json", "outputs"]))
w = next((x for x in windows if "settings" in (x["app_id"] or "").lower() or x["title"] == "Settings"), None)
if w is None:
    print("NO WINDOW; windows were:", [(x["app_id"], x["title"]) for x in windows], file=__import__("sys").stderr)
    raise SystemExit(1)
print("app id:", w["app_id"], file=__import__("sys").stderr)
layout = w["layout"]
tile = layout.get("tile_pos_in_workspace_view") or [0, 0]
offset = layout.get("window_offset_in_tile") or [0, 0]
logical = next(iter(outputs.values()))["logical"]
print(f'{tile[0] + offset[0]} {tile[1] + offset[1]}|{logical["width"]} {logical["height"]}')
PY
}
for _ in $(seq 1 120); do grep -q "SETTINGS agents" "$LOG" && break; sleep 0.25; done
echo "--- 1. it becomes the manager and lists both applications"
wait_log 'status=connected clients=2' || { fail "not connected as manager"; cat "$SCRATCH/state/helix-service.log" | tail -3; exit 1; }
sleep 2
niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png 2>/dev/null | head -1 | xargs -I{} cp {} "$SCRATCH/settings-step1.png"
loc=$(locate_window); export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}; echo "window at $UI_ORIGIN on a screen of $UI_SCREEN"
setting_of() {  # the service's own answer for one setting
  python3 - "$1" <<'PY'
import sys, subprocess, re
out = subprocess.check_output(["gdbus", "call", "--session", "--dest", "org.helix.Desktop", "--object-path", "/org/helix/Desktop", "--method", "org.helix.Desktop.Configuration1.GetSettings"], text=True)
m = re.search(r"'id': <'%s'>.*?'value': <([^>]*)>" % re.escape(sys.argv[1]), out)
print(m.group(1) if m else "<missing>")
PY
}
echo "--- 1b. the Appearance page changes service settings"
for _ in $(seq 1 20); do has_id set-theme.mode-light && break; sleep 0.25; done
has_id set-theme.mode-light && has_id set-theme.pure_black_dark && echo "ok: appearance rows listed" || fail "appearance rows missing"
echo "mode before: $(setting_of theme.mode)"
click_id set-theme.mode-light; sleep 1.2
[ "$(setting_of theme.mode)" = "'light'" ] && echo "ok: theme mode set to light" || fail "theme mode is $(setting_of theme.mode)"
click_id set-theme.pure_black_dark; sleep 1.2
[ "$(setting_of theme.pure_black_dark)" = "true" ] && echo "ok: pure black on" || fail "pure black is $(setting_of theme.pure_black_dark)"
niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png 2>/dev/null | head -1 | xargs -I{} cp {} "$SCRATCH/settings-appearance.png"
echo "--- 1c. the Keyboard page lists the key binds and flags the clash"
click_id page-keyboard; sleep 1.2
for _ in $(seq 1 20); do has_id "shortcut:Open the launcher" && break; sleep 0.25; done
has_id "shortcut:Open the launcher" && has_id "shortcut:Open a Terminal" && has_id "shortcut:Close window" && echo "ok: binds listed" || fail "binds missing"
niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png 2>/dev/null | head -1 | xargs -I{} cp {} "$SCRATCH/settings-keyboard.png"
echo "--- 1d. the Wallpaper page writes the choice the wallpaper process reads"
CHOICE=$SCRATCH/config/helix/wallpaper.toml
click_id page-wallpaper; sleep 1.5
for _ in $(seq 1 20); do has_id wp-image-1 && break; sleep 0.25; done
has_id wp-image-2 && echo "ok: three pictures listed" || fail "pictures missing"
sleep 1; niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png 2>/dev/null | head -1 | xargs -I{} cp {} "$SCRATCH/settings-wallpaper.png"
click_id wp-image-1; sleep 1
grep -q 'beta-green.png' "$CHOICE" 2>/dev/null && echo "ok: the second picture was chosen" || fail "choice is: $(cat "$CHOICE" 2>/dev/null)"
[ "$(stat -c %a "$CHOICE" 2>/dev/null)" = 600 ] && echo "ok: the file is owner only" || fail "file mode is $(stat -c %a "$CHOICE" 2>/dev/null)"
click_id wp-swatch-3; sleep 1
grep -q 'color:#1f4d3a' "$CHOICE" 2>/dev/null && echo "ok: a colour was chosen" || fail "choice is: $(cat "$CHOICE" 2>/dev/null)"
click_id page-agents; sleep 0.8
for _ in $(seq 1 20); do has_id agent-0 && has_id agent-1 && break; sleep 0.25; done
has_id agent-0 && has_id agent-1 && echo "ok: two applications listed" || fail "the applications are not on the page"

echo "--- 1d2. the keyboard: / searches across pages, Escape leaves the search"
click_id page-appearance; sleep 0.8
wtype "/"; sleep 0.3; wtype "contrast"; sleep 0.8
has_id set-accessibility.high_contrast && echo "ok: the search found a setting on another page" || fail "search found nothing for contrast"
wtype -k Escape; sleep 0.6
has_id set-accessibility.high_contrast && fail "Escape did not leave the search" || echo "ok: Escape went back to the page"
click_id page-agents; sleep 0.6

echo "--- 1d3. a number can be typed (and an out-of-range one is not sent); the lock time goes to the file helix-idle reads"
IDLE=$SCRATCH/config/helix/idle.toml
click_id page-power; sleep 1
niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png 2>/dev/null | head -1 | xargs -I{} cp {} "$SCRATCH/settings-power.png"
for _ in $(seq 1 20); do has_id set-lock_minutes-number && break; sleep 0.25; done
click_id set-lock_minutes-number; sleep 0.3
wtype "10"; wtype -k Return; sleep 1.2
grep -q 'lock_after_seconds = 600' "$IDLE" 2>/dev/null && echo "ok: 10 minutes was written" || fail "idle.toml: $(cat "$IDLE" 2>&1)"
click_id set-lock_minutes-number; wtype "999"; wtype -k Return; sleep 1
grep -q 'lock_after_seconds = 600' "$IDLE" 2>/dev/null && echo "ok: 999 is above the range and was not written" || fail "idle.toml after 999: $(cat "$IDLE" 2>&1)"
click_id page-agents; sleep 0.6

echo "--- 1e. the approvals switch writes the choice the shell reads (off by default)"
APPROVALS=$SCRATCH/config/helix/approvals.json
[ ! -f "$APPROVALS" ] && echo "ok: nothing is allowed until chosen" || fail "a choice exists already"
click_id approvals-unprotected; sleep 0.8
grep -q '"allow_unprotected":true' "$APPROVALS" && [ "$(stat -c %a "$APPROVALS")" = 600 ] && echo "ok: allowed, in an owner-only file" || fail "approvals file: $(cat "$APPROVALS" 2>/dev/null)"
click_id approvals-unprotected; sleep 0.8
grep -q '"allow_unprotected":false' "$APPROVALS" && echo "ok: switched back off" || fail "not off: $(cat "$APPROVALS" 2>/dev/null)"

echo "--- 2. a tier switch changes what the service holds"
before=$(tiers_of "exe:/opt/agents/helper.py"); echo "helper before: $before"
click_id tier-0-observe-private; sleep 1.2
after=$(tiers_of "exe:/opt/agents/helper.py"); echo "helper after: $after"
[ "$after" = "act,observe,observe-private" ] && echo "ok: observe-private granted, the others kept" || fail "wrong tiers: $after"
click_id tier-0-act; sleep 1.2
[ "$(tiers_of 'exe:/opt/agents/helper.py')" = "observe,observe-private" ] && echo "ok: act withdrawn" || fail "act not withdrawn: $(tiers_of 'exe:/opt/agents/helper.py')"
[ "$(tiers_of 'exe:/opt/agents/notes-agent')" = "observe" ] && echo "ok: the other application is untouched" || fail "the other application changed"

echo "--- 3. revoking removes the application"
click_id revoke-1; sleep 1.2
[ "$(tiers_of 'exe:/opt/agents/notes-agent')" = "<none>" ] && echo "ok: revoked" || fail "still has: $(tiers_of 'exe:/opt/agents/notes-agent')"
wait_log 'clients=1' && echo "ok: the page followed" || fail "the page still lists it"

echo "--- 4. the changes are in the audit"
grep -c '"tool":"grant-change"' "$SCRATCH/state/noctalia/agent/audit.jsonl" | sed 's/^/grant-change records: /'
[ "$(grep -c '"tool":"grant-change"' "$SCRATCH/state/noctalia/agent/audit.jsonl")" -ge 3 ] && echo "ok: every change was recorded" || fail "changes missing from the audit"
niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png 2>/dev/null | head -1 | xargs -I{} cp {} "$SCRATCH/settings-final.png"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
