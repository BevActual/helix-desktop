#!/bin/bash
# Helix Software against a fake catalog: browse, search, open a package, install with progress, update all, remove, sources.
#   tools/nested/ui-suite.sh software
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { rm -f "$SCRATCH"/shots/*.png; niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
LOG=$SCRATCH/program.log
for _ in $(seq 1 80); do has_id page-discover && break; sleep 0.25; done
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
echo "--- 1. discover"
has_id page-discover && has_id page-library && has_id page-updates && has_id page-sources && echo "ok: sidebar pages" || fail "pages missing"
wait_id hero && echo "ok: a featured app" || fail "no hero"
wait_id "shelf:recent" && echo "ok: a Recently updated shelf" || fail "no recent shelf"
wait_id "shelf:Game" && echo "ok: category shelves" || fail "no game shelf"
wait_id "tile:recent:org.helix.Demo" && echo "ok: apps are on the shelves" || fail "demo missing"
has_id page-updates-count && echo "ok: the update count is on the sidebar" || fail "no update count"
has_id cat-Game && has_id cat-Office && echo "ok: categories in the sidebar" || fail "no categories"
shot software-discover
echo "--- 2. a category"
has_id "see-all:Game" && echo "ok: a shelf offers See All" || fail "no See All"
click_id cat-Game; sleep 1.2
wait_id "pkg:org.extra.Game000" && echo "ok: the category page lists its apps" || fail "category page missing rows"
has_id "pkg:org.extra.Game099" && fail "category was not windowed" || echo "ok: the category list is windowed"
shot software-category
echo "--- 3. search"
click_id page-discover; sleep 1
click_id search; wtype "notes"; sleep 1
wait_id "pkg:org.helix.Notes" && echo "ok: search finds the app" || fail "search did not find it"
click_id page-discover; sleep 1
echo "--- 4. open a package"
wait_id "tile:recent:org.helix.Demo" || fail "demo missing"
click_id "tile:recent:org.helix.Demo" 0.3; sleep 1
wait_id detail && echo "ok: the detail page opened" || fail "no detail page"
wait_id description && echo "ok: the description is shown" || fail "no description"
wait_id screenshots && echo "ok: screenshots are shown" || fail "no screenshots"
shot software-detail
echo "--- 5. install with progress"
click_id detail-action; sleep 1
wait_id confirm-yes && echo "ok: install asks first" || fail "no confirm"
click_id confirm-yes; sleep 0.8
wait_id "progress:org.helix.Demo" && echo "ok: the ring shows progress" || fail "no ring"
shot software-progress
wait_id "open:org.helix.Demo" 60 && echo "ok: the install finished and offers Open" || fail "install did not finish"
grep -q "SOFTWARE fake Install org.helix.Demo" "$LOG" && echo "ok: the backend ran once" || fail "backend did not run"
wait_id detail-open && echo "ok: the detail page now offers Open" || fail "detail not updated"
echo "--- 6. back and the library"
click_id back; sleep 0.8
click_id page-library; sleep 1
wait_id "pkg:org.helix.Demo" && echo "ok: the app is in the library" || fail "not in library"
echo "--- 7. update all"
click_id page-updates; sleep 1
wait_id update-all && echo "ok: Update All is offered" || fail "no Update All"
click_id update-all; sleep 1
wait_id confirm-yes && click_id confirm-yes; sleep 0.5
wait_id "job:*" 40 && echo "ok: the update job runs" || fail "no update job"
for _ in $(seq 1 60); do has_id "pkg:org.helix.Editor" || break; sleep 0.25; done
has_id "pkg:org.helix.Editor" && fail "the updated app is still listed as needing an update" || echo "ok: nothing left to update"
echo "--- 8. sources"
click_id page-sources; sleep 1
wait_id "source:flathub" && echo "ok: sources are listed" || fail "sources missing"
[ "$FAILED" = 0 ] && echo "PASS: software nested" || exit 1
