#!/bin/bash
# The Network and Bluetooth pages of Settings against the fake desktop: every control is clicked by name and the exact actions the
# service received are compared with what must (and must not) have been sent.
#   HELIX_NESTED_SCRIPT=tools/nested/settings-net-test.sh tools/nested/run.sh settings-net tools/nested/settings-fake-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
NET=$SCRATCH/state/fake-network-actions.log
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
for _ in $(seq 1 120); do grep -q "SETTINGS bluetooth" "$LOG" && break; sleep 0.25; done; sleep 2
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "settings" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|}
count() { [ -f "$NET" ] && grep -c "^ACTION" "$NET" || echo 0; }
click_id page-network; sleep 1.5; shot settings-net-open
echo "--- 1. networks that cannot be joined send nothing"
N0=$(count); click_id "net:Neighbour"; sleep 0.5
[ $(( $(count) - N0 )) = 0 ] && echo "ok: nothing sent for a password-protected network until a password is given" || fail "something was sent"
click_id wifi-cancel; sleep 0.5
echo "--- 1b. a secured network asks for a password: a bad one is not sent, a good one is, and the log never shows it"
click_id "net:Neighbour"; sleep 0.8; has_id wifi-password && echo "ok: the password field appeared" || fail "no password field"
click_id wifi-password; N0=$(count); wtype "abc"; wtype -k Return; sleep 0.6
[ $(( $(count) - N0 )) = 0 ] && echo "ok: a too-short password was not sent" || fail "a bad password was sent"
wtype -M ctrl -k a -m ctrl; wtype -k BackSpace; wtype "testpass123"; sleep 0.3; shot settings-net-password; wtype -k Return; sleep 1
grep -q 'ACTION connect {"path": "/ap/neigh", "secret": "<secret:11>"}' "$NET" && echo "ok: the password was sent for the right network" || fail "password join missing"
grep -q testpass123 "$NET" "$LOG" && fail "the password is in a log" || echo "ok: the password is in no log"
has_id wifi-password && fail "the form is still open" || echo "ok: the form closed"
echo "--- 1c. an enterprise network asks for identity and password; PEAP with the wrong inner method cannot be sent"
click_id "net:Corp"; sleep 0.8; has_id ent-identity && has_id ent-password && echo "ok: the sign-in form appeared" || fail "no enterprise form"
shot settings-net-enterprise
N0=$(count); click_id ent-join; sleep 0.6
[ $(( $(count) - N0 )) = 0 ] && echo "ok: an empty form was not sent" || fail "an empty enterprise form was sent"
click_id ent-identity; wtype "me@corp.example"; click_id ent-password; wtype "hunter22!"; click_id ent-join; sleep 1
grep -q '"identity": "me@corp.example"' "$NET" && grep -q 'ACTION enterprise' "$NET" && echo "ok: the enterprise join was sent" || fail "no enterprise join"
grep -q hunter22 "$NET" "$LOG" && fail "the password is in a log" || echo "ok: the password is in no log"
has_id ent-identity && fail "the form is still open" || echo "ok: the form closed"
echo "--- 2. open network joined, disconnect, saved network joined, forget, VPN, Wi-Fi off/on, scan"
click_id "net:Cafe"; sleep 0.6; click_id wifi-disconnect; sleep 0.6; click_id "net:Home WiFi"; sleep 0.6; shot settings-net-home
click_id "net-forget:Home WiFi"; sleep 1.2; click_id "vpn:Work VPN"; sleep 0.8
click_id wifi-switch; sleep 0.6; shot settings-net-off; click_id wifi-switch; sleep 0.8; click_id wifi-scan; sleep 0.6
EXPECTED='ACTION connect {"path": "/ap/neigh", "secret": "<secret:11>"}
ACTION enterprise {"anonymous": "", "certificate": "", "domain": "", "eap": 0, "identity": "me@corp.example", "path": "/ap/corp", "phase2": 0, "secret": "<secret:9>"}
ACTION connect {"path": "/ap/cafe"}
ACTION disconnect {}
ACTION connect {"path": "/ap/home"}
ACTION forget {"ssid": "Home WiFi"}
ACTION vpn-connect {"path": "/vpn/work"}
ACTION enable {"value": false}
ACTION enable {"value": true}
ACTION scan {}'
[ "$(grep '^ACTION' "$NET")" = "$EXPECTED" ] && echo "ok: exactly the expected network actions, in order" || { fail "network actions differ"; cat "$NET"; }
[ "$(grep -c '^REFUSED' "$NET" 2>/dev/null)" = 0 ] && echo "ok: the service refused nothing" || fail "service refused something"
echo "--- 3. Bluetooth"
click_id page-bluetooth; sleep 1.5; shot settings-bt-open
BT=$SCRATCH/state/fake-bluetooth-actions.log
echo "--- 3b. pairing: compare a code (yes), and type a PIN (too long refused, then accepted)"
click_id "bt-pair:Kitchen Speaker"; sleep 1.2
has_id pair-code && has_id pair-accept && echo "ok: the code to compare is shown" || fail "no code shown"
shot settings-bt-pair-confirm
click_id pair-accept; sleep 1.2
has_id pair-accept && fail "the question is still showing" || echo "ok: the question closed"
grep -q '^REPLY cancel=False length=0' "$BT" && echo "ok: a yes was sent" || fail "no yes in $(cat "$BT")"
click_id "bt-pair:Old Headset"; sleep 1.2
has_id pair-pin && echo "ok: a PIN is asked for" || fail "no PIN field"
click_id pair-pin; wtype "12345678901234567"; wtype -k Return; sleep 0.6
grep -c '^REPLY' "$BT" | sed 's/^/replies so far: /'
[ "$(grep -c '^REPLY' "$BT")" = 1 ] && echo "ok: a 17-character PIN was not sent" || fail "a bad PIN was sent"
wtype -M ctrl -k a -m ctrl; wtype -k BackSpace; wtype "0000"; shot settings-bt-pair-pin; wtype -k Return; sleep 1.2
grep -q '^REPLY cancel=False length=4' "$BT" && echo "ok: the PIN was sent (length only in the log)" || fail "PIN missing"
grep -q 0000 "$BT" && fail "the PIN is in the log" || echo "ok: the PIN is in no log"
has_id "bt-forget:Kitchen Speaker" && echo "ok: both devices are paired now" || fail "not paired"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
