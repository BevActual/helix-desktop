#!/bin/bash
# Regenerate with another model: a menu beside Regenerate redoes the last reply with the chosen model for that reply only.
#   HELIX_NESTED_SCRIPT=tools/nested/mobius-regen-test.sh tools/nested/run.sh mobius-regen tools/nested/mobius-nested.sh <wide window>
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
idle() { sleep 2; for _ in $(seq 1 120); do ! has_id stop && break; sleep 0.5; done; sleep 1.5; }
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
CHAT() { python3 - "$SCRATCH/data/helix/mobius/chats" "$@" <<'PY'
import glob, json, sys
c = json.load(open(glob.glob(sys.argv[1] + "/*.json")[0]))
m = c["messages"][-1]
print(c.get("route_backend", ""), c.get("route_model", ""), (m.get("stats") or {}).get("model", ""), len(c["messages"]), sep="|")
PY
}
click_id input; wtype "Hello there"; wtype -k Return; idle
[ "$(CHAT)" = "||fake-model|2" ] && echo "ok: the first reply came from the default model" || echo "note: first reply: $(CHAT)"
wait_id regenerate && wait_id regenerate-more && echo "ok: Regenerate has a menu beside it" || fail "no regenerate menu button"
click_id regenerate-more
wait_id regen-menu && wait_id regen-model-fake-model && echo "ok: the menu lists the installed models" || fail "menu missing"
shot regen; sleep 1; shot regen
click_id regen-model-fake-model; idle
R=$(CHAT)
[ "${R##*|}" = "2" ] && echo "ok: the reply was replaced, not added to" || fail "message count: $R"
[ "$(echo "$R" | cut -d"|" -f3)" = "fake-model" ] && echo "ok: the new reply says it was written by the chosen model" || fail "reply model: $R"
case "$R" in "||"*) echo "ok: the chat's own model choice was not changed";; *) fail "the chat's route changed: $R";; esac
echo "--- the replaced answer is kept, and the two can be compared"
wait_id compare-bar 20 && echo "ok: the old answer was kept (Compare is offered)" || fail "no compare bar"
click_id compare-open
wait_id compare-view && wait_id compare-current && wait_id compare-other && echo "ok: the two answers are side by side" || fail "no compare view"
shot compare; sleep 1; shot compare
click_id compare-keep-other; sleep 2
R2=$(CHAT)
[ "$(echo "$R2" | cut -d"|" -f3)" = "" ] && echo "ok: Keep this one swapped the other answer in (its model is now on the reply)" || fail "after keep: $R2"
wait_id compare-bar 20 && echo "ok: and the one it replaced is kept, so the choice can be undone" || fail "no alternative after the swap"
[ "$FAILED" = 0 ] && echo "PASS: mobius regen" || exit 1
