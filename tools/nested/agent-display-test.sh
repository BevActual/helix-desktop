#!/bin/bash
# The agent display: a second headless compositor that only an agent's programs run on. It starts with its own private runtime directory, session bus
# and home; an application launched onto it is a window there and nowhere on the person's desktop; its environment holds nothing of the person's session;
# a picture of it can be taken; stopping it takes the program with it.
#   HELIX_NESTED_SCRIPT=tools/nested/agent-display-test.sh tools/nested/run.sh agent-display tools/nested/tauri-shell.sh HELIX_AGENT_DISPLAY_NIRI=$PWD/target/fast/niri
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; CONTROL=$SCRATCH/state/surface-control; FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
wait_log() { for _ in $(seq 1 ${2:-120}); do grep -q "$1" "$LOG" && return 0; sleep 0.25; done; return 1; }
RT=$SCRATCH/state/run/helix-agent-display
# An installed application to launch: Settings, through a desktop file of this test's own.
mkdir -p "$XDG_DATA_HOME/applications"
printf '[Desktop Entry]\nType=Application\nName=Agent demo\nExec="%s/target/fast/helix-settings"\n' "$ROOT" > "$XDG_DATA_HOME/applications/agentdemo.desktop"
for _ in $(seq 1 120); do grep -q "READY shell" "$LOG" && break; sleep 0.25; done; sleep 2
echo "--- start"
echo agent-display-start >> "$CONTROL"
wait_log "AGENT-DISPLAY started" 80 && echo "ok: the display started" || fail "no start: $(grep AGENT "$LOG")"
perm=$(stat -c %a "$RT" 2>/dev/null); [ "$perm" = 700 ] && echo "ok: its runtime directory is private (mode 700)" || fail "runtime dir mode $perm"
ASOCK=$(ls "$RT"/niri.*.sock 2>/dev/null | head -1)
[ -S "$ASOCK" ] && echo "ok: its compositor listens inside that directory" || fail "no compositor socket in $RT"
[ -S "$RT/bus" ] && echo "ok: it has its own session bus" || fail "no private bus"
echo "--- an application on the display"
echo "agent-display-launch agentdemo.desktop" >> "$CONTROL"
wait_log "AGENT-DISPLAY launched agentdemo.desktop" 40 || fail "not launched: $(grep AGENT "$LOG")"
for _ in $(seq 1 60); do NIRI_SOCKET=$ASOCK niri msg --json windows 2>/dev/null | grep -q "org.helix.Settings" && break; sleep 0.5; done
NIRI_SOCKET=$ASOCK niri msg --json windows | grep -q "org.helix.Settings" && echo "ok: Settings is a window on the agent display" || fail "no window on the display"
mine=$(niri msg --json windows | python3 -c 'import json,sys; print(len([w for w in json.load(sys.stdin) if w["app_id"]=="org.helix.Settings"]))')
[ "$mine" = 0 ] && echo "ok: and not on the person's desktop" || fail "a window appeared on the person's desktop"
# The program on the display is the Settings process whose environment names the display's runtime directory.
find_program() { for p in $(pgrep -f "target/fast/helix-settings"); do tr '\0' '\n' < /proc/$p/environ 2>/dev/null | grep -q "^WAYLAND_DISPLAY=wayland-[0-9]*$" && tr '\0' '\n' < /proc/$p/environ 2>/dev/null | grep -q "^XDG_RUNTIME_DIR=$RT$" && { echo $p; return; }; done; }
PID=$(find_program)
[ -n "$PID" ] || fail "no program on the display"
ENV=$(tr '\0' '\n' < /proc/$PID/environ 2>/dev/null)
echo "$ENV" | grep -q "^XDG_RUNTIME_DIR=$RT$" && echo "ok: the program's runtime directory is the display's own" || fail "runtime dir: $(echo "$ENV" | grep XDG_RUNTIME_DIR)"
echo "$ENV" | grep -q "^DBUS_SESSION_BUS_ADDRESS=unix:path=$RT/bus$" && echo "ok: its session bus is the display's own" || fail "bus: $(echo "$ENV" | grep DBUS_SESSION)"
echo "$ENV" | grep -q "^NIRI_SOCKET=" && fail "it inherited a compositor socket" || echo "ok: no compositor socket of the person's reaches it"
echo "$ENV" | grep -q "^HOME=.*/helix/agent-display/home$" && echo "ok: it has its own home" || fail "home: $(echo "$ENV" | grep '^HOME=')"
echo "--- a picture"
sleep 6   # the program draws a few seconds after its window appears
echo "agent-display-frame $SCRATCH/frame.png" >> "$CONTROL"
for _ in $(seq 1 40); do [ -s "$SCRATCH/frame.png" ] && break; sleep 0.5; done
python3 - "$SCRATCH/frame.png" <<'PY' && echo "ok: a picture of the display (the program is drawn on it)" || fail "bad picture"
import sys
from PIL import Image
img = Image.open(sys.argv[1]).convert("RGB"); w, h = img.size
assert (w, h) == (1280, 720), (w, h)
# Settings' dark window: the picture is not one flat colour.
colours = {img.getpixel((x, y)) for x in range(0, w, 40) for y in range(0, h, 40)}
assert len(colours) > 6, len(colours)
PY
echo "--- watching it"
wait_log "ISLAND AgentDisplay" 20 && echo "ok: the island says an agent display has windows" || fail "no island for the agent display: $(grep ISLAND "$LOG" | tail -2)"
echo agent-display >> "$CONTROL"
wait_log "AGENT-DISPLAY viewer frame 1" 40 && echo "ok: the viewer asked for pictures of it" || fail "no viewer frames"
sleep 2
python3 - "$SCRATCH/state/layout.json" <<'PY' && echo "ok: the viewer shows the picture in the panel" || fail "no picture in the viewer panel"
import json, sys
d = json.load(open(sys.argv[1]))
e = d["elements"].get("av-frame")
assert d.get("panel") == "agent-display", d.get("panel")
assert e and e["w"] > 400 and e["h"] > 200, e
PY
sleep 2; niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/viewer.png"
wtype -k Escape; sleep 1
echo "--- stop"
echo agent-display-stop >> "$CONTROL"
wait_log "AGENT-DISPLAY stopped" 20 && echo "ok: stopped" || fail "no stop"
sleep 1.5
kill -0 "$PID" 2>/dev/null && fail "the program outlived the display" || echo "ok: the program went with the display"
[ "$FAILED" = 0 ] && echo "PASS: agent display" || exit 1
