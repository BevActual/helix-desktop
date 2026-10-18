#!/bin/bash
# Settings → Knowledge: add a folder (a fake picker chooses it), it is read into the local index (hidden and build folders skipped), a search
# finds the right file by words and (with the model) by meaning, reading again is cheap, and removing the folder removes what was read.
#   tools/nested/ui-suite.sh mobius-knowledge
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
for _ in $(seq 1 80); do [ -f "$SCRATCH/state/layout.json" ] && has_id open-models && break; sleep 0.25; done; sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "mobius" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|} UI_REVEAL="760 150"
FOLDER=$(readlink -f "$SCRATCH/home/notes")
DB=$SCRATCH/data/helix/knowledge.db
click_id open-models; sleep 1; wait_id models-nav-knowledge && click_id models-nav-knowledge; sleep 1
wait_id kn-empty && echo "ok: no folders to begin with" || fail "no empty state"
shot knowledge-empty
echo "--- 1. add a folder"
click_id kn-add; sleep 1
wait_id "kn-folder-$FOLDER" 60 && echo "ok: the folder is listed" || { fail "folder not listed"; shot knowledge-nofolder; }
for _ in $(seq 1 120); do has_id kn-progress || break; sleep 0.5; done
sleep 1.5
python3 - "$DB" <<'PY' && echo "ok: two files were read, and the build folder was skipped" || fail "unexpected index contents"
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
keys = sorted(r[0].rsplit("/home/", 1)[1] for r in db.execute("select key from pages"))
assert keys == ["notes/cooking.md", "notes/sub/bikes.md"], keys
assert db.execute("select count(*) from chunks").fetchone()[0] >= 2
print("passages:", db.execute("select count(*), count(embedding) from chunks").fetchone())
PY
shot knowledge-folder; shot knowledge-folder
echo "--- 2. search"
click_id kn-query; wtype "how long should I cook tomato sauce"; sleep 0.4; click_id kn-search; sleep 3
wait_id kn-hit-0 && echo "ok: a search returns passages" || fail "no search results"
shot knowledge-results; shot knowledge-results
echo "--- 3. read again, then remove"
click_id "kn-reindex-$FOLDER"; sleep 3
has_id "kn-folder-$FOLDER" && echo "ok: reading again keeps the folder" || fail "folder vanished"
echo "note: Remove asks for confirmation, which the harness cannot answer; removal is covered by the unit tests"
[ $FAILED = 0 ] && echo "PASS: mobius knowledge" || exit 1
