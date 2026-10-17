#!/bin/bash
# Helix Software's Fedora backend, end to end against tools/nested/fake-packagekit.py: the catalog from AppStream data and PackageKit's installed
# and update lists, install with progress (the right architecture, once), a removal PackageKit refuses, cancelling a change, and Update All.
#   HELIX_NESTED_SCRIPT=tools/nested/software-rpm-test.sh tools/nested/run.sh software-rpm tools/nested/software-rpm-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
LOG=$SCRATCH/program.log; PK=$SCRATCH/state/fake-packagekit.log
for _ in $(seq 1 80); do has_id page-library && break; sleep 0.25; done
sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
windows = json.loads(subprocess.check_output(["niri", "msg", "--json", "windows"]))
outputs = json.loads(subprocess.check_output(["niri", "msg", "--json", "outputs"]))
w = next((x for x in windows if "software" in (x.get("app_id") or "").lower() or "Software" in (x.get("title") or "")), None)
if w is None:
    raise SystemExit("no software window")
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
echo "--- 1. the library: what PackageKit says is installed, limited to the catalog's applications"
click_id page-library; sleep 1.5
wait_id "pkg:helix-calc" && wait_id "pkg:helix-locked" && echo "ok: installed applications are listed" || fail "installed apps missing"
has_id "pkg:bash" && fail "a package that is not an application is listed" || echo "ok: bash is not an application"
has_id "pkg:helix-writer" && fail "an uninstalled app is in the library" || echo "ok: an app that is not installed is not in the library"
echo "--- 2. search finds an application that is not installed"
click_id page-discover; sleep 1; click_id search; wtype "writer"; sleep 1.2
wait_id "pkg:helix-writer" && echo "ok: the catalog is searchable" || fail "search did not find helix-writer"
has_id "pkg:helix-release" && fail "a generic component is listed" || echo "ok: generic components are left out"
echo "--- 3. install with progress, once, for this machine's architecture"
click_id "pkg:helix-writer" 0.3; sleep 1
wait_id detail && click_id detail-action; sleep 1
wait_id confirm-yes && echo "ok: install asks first" || fail "no confirm"
click_id confirm-yes; sleep 0.8
wait_id "progress:helix-writer" && echo "ok: the ring shows progress" || fail "no progress ring"
wait_id "open:helix-writer" 60 && echo "ok: the install finished and offers Open" || fail "install did not finish"
[ "$(grep -c '^INSTALL ' "$PK")" = 1 ] && grep -q '^INSTALL helix-writer$' "$PK" && echo "ok: PackageKit was asked once" || fail "installs: $(grep INSTALL "$PK")"
grep 'InstallPackages' "$PK" | grep -q 'x86_64' && ! grep 'InstallPackages' "$PK" | grep -q 'i686' && echo "ok: the native architecture was chosen" || fail "wrong architecture: $(grep InstallPackages "$PK")"
click_id back; sleep 0.8
echo "--- 4. a removal PackageKit refuses leaves the package where it was"
click_id page-library; sleep 1.2
wait_id "pkg:helix-locked" && click_id "pkg:helix-locked" 0.3; sleep 1
wait_id detail-remove && click_id detail-remove; sleep 1
wait_id confirm-yes && click_id confirm-yes; sleep 0.5
for _ in $(seq 1 40); do grep -q "SOFTWARE job helix-locked failed" "$LOG" && break; sleep 0.5; done
grep -q "SOFTWARE job helix-locked failed: Package helix-locked is needed by helix-other" "$LOG" && echo "ok: the refusal was reported in PackageKit's words" || fail "no failure reported: $(grep 'SOFTWARE job' "$LOG")"
grep -q '^REMOVE helix-locked allow_deps=False' "$PK" && echo "ok: a removal never takes other packages with it" || fail "remove call: $(grep REMOVE "$PK")"
click_id back 2>/dev/null; sleep 0.5
echo "--- 5. cancel a change in progress"
click_id page-discover; sleep 1; click_id search; wtype "Helix Calc"; sleep 1.2
wait_id "pkg:helix-calc" && click_id "pkg:helix-calc" 0.3; sleep 1
wait_id detail-remove && click_id detail-remove; sleep 1
wait_id confirm-yes && click_id confirm-yes; sleep 0.9
wait_id "cancel:helix-calc" 20 && click_id "cancel:helix-calc"; sleep 2.5
for _ in $(seq 1 20); do grep -q '^CANCELLED' "$PK" && break; sleep 0.5; done
grep -q '^CANCELLED' "$PK" && echo "ok: Cancel reached PackageKit" || fail "no cancel: $(cat "$PK" | tail -5)"
grep -q "SOFTWARE job helix-calc failed" "$LOG" && fail "a cancelled change was reported as a failure" || echo "ok: cancelling is not a failure"
click_id back 2>/dev/null; sleep 0.5
echo "--- 6. Update All: the application's update and the system software, in one confirmation"
click_id page-updates; sleep 1.5
wait_id "pkg:helix-calc" && wait_id "pkg:system-updates" && echo "ok: an application update and one System software row" || fail "update rows missing"
click_id update-all; sleep 1
wait_id confirm-yes && click_id confirm-yes; sleep 0.5
for _ in $(seq 1 60); do grep -q '^UPDATE ' "$PK" && break; sleep 0.5; done
grep -q '^UPDATE glibc helix-calc kernel$' "$PK" && echo "ok: one PackageKit update for everything that had one" || fail "update call: $(grep UPDATE "$PK")"
for _ in $(seq 1 80); do has_id "pkg:helix-calc" || break; sleep 0.25; done
has_id "pkg:helix-calc" && fail "the updated app is still listed" || echo "ok: nothing is left to update"
[ "$FAILED" = 0 ] && echo "PASS: software rpm" || exit 1
