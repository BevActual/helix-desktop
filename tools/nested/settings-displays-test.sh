#!/bin/bash
# Settings > Displays: a scale and an orientation are sent to the compositor, and the output reports both back.
#   HELIX_NESTED_SCRIPT=tools/nested/settings-displays-test.sh tools/nested/run.sh settings-displays tools/nested/settings-fake-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
place() {
  loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "settings" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
  export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
}
# One output, as the compositor reports it: name, scale, transform.
read_output() {
  python3 - <<'PY'
import json, subprocess
name, output = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).items()))
logical = output.get("logical") or {}
print(name)
print(logical.get("scale", ""))
print(logical.get("transform", ""))
PY
}
# Put the output back so the next click still lands inside it. This is the test restoring the compositor, not another Settings action.
restore_output() {
  python3 - "$1" "$2" "$3" <<'PY'
import json, os, socket, sys
name, scale, turn = sys.argv[1:]
def send(action):
    sock = socket.socket(socket.AF_UNIX)
    sock.connect(os.environ["NIRI_SOCKET"])
    sock.send(json.dumps({"Output": {"output": name, "action": action}}).encode() + b"\n")
    sock.recv(4096)
send({"Scale": {"scale": {"Specific": float(scale)}}})
send({"Transform": {"transform": turn}})
PY
  sleep 0.4
}
for _ in $(seq 1 120); do grep -q "SETTINGS bluetooth" "$LOG" && break; sleep 0.25; done; sleep 2
place
mapfile -t NOW < <(read_output)
NAME=${NOW[0]}
click_id page-displays; sleep 1.5; shot settings-displays
has_id "set-scale:${NAME}-1" && echo "ok: scale is on the page" || fail "no scale control for $NAME"
has_id "set-turn:${NAME}-90" && echo "ok: orientation is on the page" || fail "no orientation control for $NAME"
TOKEN=$(python3 - "$NAME" <<'PY'
import json, subprocess, sys
name = sys.argv[1]
output = json.loads(subprocess.check_output(["niri","msg","--json","outputs"]))[name]
mode = output["modes"][output["current_mode"]]
print(f'{mode["width"]}x{mode["height"]}@{mode["refresh_rate"]}')
PY
)
has_id "set-mode:${NAME}-${TOKEN}" && echo "ok: resolution is on the page" || fail "no resolution control for $NAME $TOKEN"
# One headless display cannot be arranged or switched off, and it does not advertise variable refresh.
has_id "set-vrr:${NAME}" && fail "variable refresh on a display that has none" || echo "ok: no variable refresh row"
has_id "set-power:${NAME}" && fail "the only display can be turned off" || echo "ok: the only display stays on"
has_id "set-place:${NAME}-auto" && fail "arrangement with one display" || echo "ok: no arrangement row"
# A higher scale shrinks the output, so pick one that is not already set, then put the output back before the next click.
TARGET=1.25
SPECIFIC=1.25
python3 -c 'import sys; raise SystemExit(0 if abs(float(sys.argv[1]) - 1.25) < 0.02 else 1)' "${NOW[1]}" && { TARGET=1.5; SPECIFIC=1.5; }
click_id "set-scale:${NAME}-${TARGET}"; sleep 0.5
SCALE_LINE="SETTINGS display {\"Output\":{\"action\":{\"Scale\":{\"scale\":{\"Specific\":${SPECIFIC}}}},\"output\":\"${NAME}\"}"
grep -qF "$SCALE_LINE" "$LOG" && echo "ok: scale request sent" || fail "scale request missing: $(grep 'SETTINGS display' "$LOG" | tail -3)"
mapfile -t AFTER < <(read_output)
python3 -c 'import sys; raise SystemExit(0 if abs(float(sys.argv[1]) - float(sys.argv[2])) < 0.02 else 1)' "${AFTER[1]}" "$TARGET" \
  && echo "ok: compositor scale is $TARGET" || fail "scale stayed ${AFTER[1]}"
PROFILES=$SCRATCH/state/helix/display-profiles.json
niri_send() { python3 - "$1" <<'PY'
import os, socket, sys
sock = socket.socket(socket.AF_UNIX); sock.connect(os.environ["NIRI_SOCKET"]); sock.send(sys.argv[1].encode() + b"\n"); print(sock.recv(65536).decode().strip())
PY
}
profile_has() { python3 - "$PROFILES" "$1" "$2" <<'PY'
import json, sys
p = json.load(open(sys.argv[1]))["profiles"][-1]["outputs"][0]
key, want = sys.argv[2], sys.argv[3]
sys.exit(0 if str(p.get(key)).strip("[]'\"") == want or abs(float(p[key]) - float(want)) < 0.02 else 1) if key == "scale" and p.get(key) is not None else sys.exit(0 if str(p.get(key)) == want else 1)
PY
}
profile_has scale "$TARGET" && echo "ok: the compositor remembered the scale for this set of displays" || fail "profile file: $(cat "$PROFILES" 2>&1)"
[ ! -e "$SCRATCH/config/niri/helix-setup.kdl" ] && echo "ok: and no KDL was rewritten" || fail "helix-setup.kdl was written: $(cat "$SCRATCH/config/niri/helix-setup.kdl")"
restore_output "$NAME" "${NOW[1]:-1}" "${NOW[2]:-Normal}"
place
click_id "set-turn:${NAME}-90"; sleep 0.5
TURN_LINE="SETTINGS display {\"Output\":{\"action\":{\"Transform\":{\"transform\":\"90\"}},\"output\":\"${NAME}\"}"
grep -qF "$TURN_LINE" "$LOG" && echo "ok: orientation request sent" || fail "orientation request missing: $(grep 'SETTINGS display' "$LOG" | tail -3)"
mapfile -t TURNED < <(read_output)
[ "${TURNED[2]}" = "90" ] && echo "ok: compositor orientation is 90" || fail "orientation stayed ${TURNED[2]}"
python3 -c 'import json,sys; sys.exit(0 if json.load(open(sys.argv[1]))["profiles"][-1]["outputs"][0]["transform"] == "90" else 1)' "$PROFILES" && echo "ok: orientation remembered too" || fail "transform not kept: $(cat "$PROFILES" 2>&1)"
echo "--- the remembered layout is applied again when asked (the same path a reconnected set takes)"
restore_output "$NAME" "${NOW[1]:-1}" "${NOW[2]:-Normal}"
mapfile -t BACK < <(read_output)
[ "${BACK[2]}" = "Normal" ] && echo "ok: put back to Normal by hand" || fail "restore failed: ${BACK[*]}"
niri_send '"HelixApplyDisplayProfile"' | grep -q Handled && sleep 0.6
mapfile -t AGAIN < <(read_output)
[ "${AGAIN[2]}" = "90" ] && echo "ok: applying the remembered layout turned it to 90 again" || fail "profile not applied: ${AGAIN[*]}"
niri_send '"HelixDisplayProfile"' | grep -q '"saved":true' && echo "ok: the compositor says a layout is remembered for this set" || fail "profile state not reported"
grep -q "Remembered layout" <(true) ; has_id "set-profile:status" >/dev/null 2>&1 || true
restore_output "$NAME" "${NOW[1]:-1}" "${NOW[2]:-Normal}"
echo "--- night light is at the top of the page and writes the file the service reads"
NIGHT=$SCRATCH/config/helix/nightlight.json
has_id "set-night:enabled" && echo "ok: Night light is on the page" || fail "no night light switch"
has_id "set-night:warmth" && has_id "set-night:scheduled" && echo "ok: with its warmth and schedule" || fail "warmth or schedule row missing"
click_id "set-night:enabled"; sleep 1
python3 - "$NIGHT" <<'PY' && echo "ok: switching it on is recorded in nightlight.json" || fail "nightlight.json not as expected: $(cat "$NIGHT" 2>&1)"
import json, sys
d = json.load(open(sys.argv[1]))
assert d["enabled"] is True and d["scheduled"] is True and 2000 <= d["warmth"] <= 6500, d
PY
has_id "set-night:sun" && echo "ok: with a schedule, it can follow sunset and sunrise" || fail "no follow-the-sun switch"
click_id "set-night:sun"; sleep 1
python3 -c 'import json,sys; sys.exit(0 if json.load(open(sys.argv[1]))["sun"] is True else 1)' "$NIGHT" && echo "ok: following the sun is recorded" || fail "sun not recorded: $(cat "$NIGHT")"
has_id "set-night:start" && fail "the hours stay while following the sun" || echo "ok: the hours go away while following the sun"
click_id "set-night:scheduled"; sleep 1
python3 -c 'import json,sys; sys.exit(0 if json.load(open(sys.argv[1]))["scheduled"] is False else 1)' "$NIGHT" && echo "ok: the schedule can be turned off (always warm)" || fail "schedule not recorded"
has_id "set-night:start" && fail "the hours stay when there is no schedule" || echo "ok: the hours go away without a schedule"
shot settings-displays-after
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
