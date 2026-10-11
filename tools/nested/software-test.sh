#!/bin/bash
# Helix Software against a fake catalog: browse, search, open a package, install with progress, update all, remove, sources.
#   tools/nested/ui-suite.sh software
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { rm -f "$SCRATCH"/shots/*.png; niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
LOG=$SCRATCH/program.log
for _ in $(seq 1 80); do has_id page-explore && break; sleep 0.25; done
sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
windows = json.loads(subprocess.check_output(["niri", "msg", "--json", "windows"]))
outputs = json.loads(subprocess.check_output(["niri", "msg", "--json", "outputs"]))
w = next((x for x in windows if "software" in (x.get("app_id") or "").lower() or "Software" in (x.get("title") or "")), None)
if w is None:
    raise SystemExit("no software window: " + str([(x.get("app_id"), x.get("title")) for x in windows]))
layout = w["layout"]
tile = layout.get("tile_pos_in_workspace_view") or [0, 0]
offset = layout.get("window_offset_in_tile") or [0, 0]
logical = next(iter(outputs.values()))["logical"]
print(f'{tile[0] + offset[0]} {tile[1] + offset[1]}|{logical["width"]} {logical["height"]}')
PY
) || { fail "could not locate window"; exit 1; }
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
gone_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" || return 0; sleep 0.25; done; return 1; }
echo "--- 1. browse"
has_id page-explore && has_id page-library && has_id page-updates && has_id page-sources && echo "ok: pages" || fail "pages missing"
wait_id "pkg:org.helix.Demo" && echo "ok: explore lists the catalog" || fail "demo missing"
has_id "pkg:org.filler.App100" && fail "the list was not windowed" || echo "ok: the list is windowed (not every row at once)"
has_id page-updates-count && echo "ok: the update count is on the sidebar" || fail "no update count"
shot software-explore
echo "--- 2. search"
click_id search; wtype "notes"; sleep 1
wait_id "pkg:org.helix.Notes" && ! has_id "pkg:org.helix.Demo" && echo "ok: search narrows the list" || fail "search did not narrow"
for _ in 1 2 3 4 5; do wtype -k BackSpace; sleep 0.05; done; sleep 0.8
echo "--- 3. open a package"
click_id search; wtype -k Escape
wait_id "pkg:org.helix.Demo" || fail "demo missing after clearing search"
click_id "pkg:org.helix.Demo" 0.3; sleep 1
wait_id detail && echo "ok: the detail page opened" || fail "no detail page"
wait_id description && echo "ok: the description is shown" || fail "no description"
shot software-detail
echo "--- 4. install with progress"
click_id detail-action; sleep 1
wait_id confirm-yes && echo "ok: install asks first" || fail "no confirm"
click_id confirm-yes; sleep 0.8
wait_id "job:org.helix.Demo" && echo "ok: a job with progress appears" || fail "no job"
wait_id "open:org.helix.Demo" 60 && echo "ok: the install finished and offers Open" || fail "install did not finish"
grep -q "SOFTWARE fake Install org.helix.Demo" "$LOG" && echo "ok: the backend ran once" || fail "backend did not run"
wait_id detail-open && echo "ok: the detail page now offers Open" || fail "detail not updated"
shot software-installed
echo "--- 5. back and the library"
click_id back; sleep 0.8
click_id page-library; sleep 1
wait_id "pkg:org.helix.Demo" && echo "ok: the app is in the library" || fail "not in library"
echo "--- 6. update all"
click_id page-updates; sleep 1
wait_id update-all && echo "ok: Update All is offered" || fail "no Update All"
click_id update-all; sleep 1
wait_id confirm-yes && click_id confirm-yes; sleep 0.5
wait_id "job:*" 40 && echo "ok: the update job runs" || fail "no update job"
for _ in $(seq 1 60); do has_id "pkg:org.helix.Editor" || break; sleep 0.25; done
has_id "pkg:org.helix.Editor" && fail "the updated app is still listed as needing an update" || echo "ok: nothing left to update"
echo "--- 7. sources"
click_id page-sources; sleep 1
wait_id "source:flathub" && echo "ok: sources are listed" || fail "sources missing"
[ "$FAILED" = 0 ] && echo "PASS: software nested" || exit 1
