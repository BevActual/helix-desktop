#!/bin/bash
# Computer use through the shell's command socket, spoken to as the desktop service speaks to it: an agent starts an application on its own display, takes a
# picture, clicks, types and presses keys there; a bad command, a flood and a request made while the person helps are refused; a help request stops the agent
# until the person says "Done, carry on"; and what the agent typed is never written to the log.
#   HELIX_NESTED_SCRIPT=tools/nested/agent-computer-use-test.sh tools/nested/run.sh agent-computer-use tools/nested/tauri-shell.sh "HELIX_AGENT_DISPLAY_NIRI=$PWD/target/fast/niri" "HELIX_AGENT_INPUT=$PWD/target/fast/helix-agent-input"
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0; STATE=$SCRATCH/state; RT=$STATE/run/helix-agent-display
fail() { echo "FAIL: $*" >&2; FAILED=1; }
wait_log() { for _ in $(seq 1 ${2:-120}); do grep -q "$1" "$LOG" && return 0; sleep 0.25; done; return 1; }
mkdir -p "$XDG_DATA_HOME/applications"
printf '[Desktop Entry]\nType=Application\nName=Agent demo\nExec="%s/target/fast/helix-settings"\n' "$ROOT" > "$XDG_DATA_HOME/applications/agentdemo.desktop"
for _ in $(seq 1 120); do grep -q "AGENT-IPC serving" "$LOG" 2>/dev/null && break; sleep 0.25; done
SOCK=$(ls "$STATE"/helix-*.sock 2>/dev/null | head -1)
[ -S "$SOCK" ] && echo "ok: the shell serves $(basename "$SOCK")" || { fail "no socket"; exit 1; }
sleep 2
send() { python3 - "$SOCK" "$1" "${2:-30}" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX); s.settimeout(float(sys.argv[3])); s.connect(sys.argv[1])
s.sendall(("/\x1e" + sys.argv[2]).encode()); s.shutdown(socket.SHUT_WR)
data = b""
while True:
    chunk = s.recv(4096)
    if not chunk: break
    data += chunk
print(data.decode().strip())
PY
}
expect_ok() { local got; got=$(send "$1"); case "$got" in ""|error:*) fail "'$1' gave '$got'";; *) echo "ok: '$1' -> ${got:0:60}";; esac; }
expect_error() { local got; got=$(send "$1"); case "$got" in error:*"$2"*) echo "ok: '$1' refused ($got)";; *) fail "'$1' gave '$got', wanted an error containing '$2'";; esac; }
pill_y() { python3 - "$1" <<'PY'
import sys
from PIL import Image
img = Image.open(sys.argv[1]).convert("RGB")
ys = [y for y in range(img.size[1]) if img.getpixel((100, y)) == (0x0a, 0x84, 0xff)]
print(round(sum(ys) / len(ys)) if ys else -1)
PY
}
picture() { local got path; got=$(send display-screenshot); path=${got#ok }; [ -s "$path" ] && { cp "$path" "$SCRATCH/$1.png"; echo "$path"; }; }
echo "--- no display yet"
expect_error "display-screenshot" "not running"
expect_error "display-click 10 10" "not running"
echo "--- start an application on the display"
expect_ok "display-launch Agent demo"
wait_log "AGENT-IPC display launched agentdemo.desktop" 60 && echo "ok: the application was started on the agent display" || fail "no launch: $(grep 'AGENT' "$LOG" | tail -3)"
for _ in $(seq 1 60); do ASOCK=$(ls "$RT"/niri.*.sock 2>/dev/null | head -1); NIRI_SOCKET=$ASOCK niri msg --json windows 2>/dev/null | grep -q org.helix.Settings && break; sleep 0.5; done
[ -n "$(NIRI_SOCKET=$ASOCK niri msg --json windows 2>/dev/null | grep org.helix.Settings)" ] && echo "ok: it is a window on the display" || fail "no window"
sleep 7
echo "--- a picture"
path=$(picture before)
[ -n "$path" ] && echo "ok: a picture of the display ($(stat -c %a "$path") mode, $(python3 -c 'from PIL import Image; import sys; print("x".join(map(str, Image.open(sys.argv[1]).size)))' "$SCRATCH/before.png"))" || fail "no picture"
[ "$(stat -c %a "$path")" = 600 ] && echo "ok: the file is private to the person" || fail "mode $(stat -c %a "$path")"
before=$(pill_y "$SCRATCH/before.png")
echo "--- clicks, keys and typing"
expect_ok "display-click 80 352"
after=$before
for _ in $(seq 1 12); do sleep 1; picture shot >/dev/null; after=$(pill_y "$SCRATCH/shot.png"); [ "$after" != "$before" ] && break; done
[ "$before" -lt 150 ] && [ "$after" -gt 330 ] && echo "ok: the click chose Keyboard (highlight $before -> $after)" || fail "highlight $before -> $after"
expect_ok "display-click 120 67"
sleep 1; picture b4 >/dev/null
expect_ok "display-type dock"
for _ in $(seq 1 8); do sleep 1; picture af >/dev/null; python3 - "$SCRATCH/b4.png" "$SCRATCH/af.png" <<'PY' && break
import sys
from PIL import Image, ImageChops
a = Image.open(sys.argv[1]).convert("RGB").crop((14, 50, 228, 86)); b = Image.open(sys.argv[2]).convert("RGB").crop((14, 50, 228, 86))
sys.exit(0 if sum(1 for p in ImageChops.difference(a, b).convert("L").getdata() if p > 60) > 40 else 1)
PY
done
python3 - "$SCRATCH/b4.png" "$SCRATCH/af.png" <<'PY' && echo "ok: the typing appeared in the search field" || fail "typing did not appear"
import sys
from PIL import Image, ImageChops
a = Image.open(sys.argv[1]).convert("RGB").crop((14, 50, 228, 86)); b = Image.open(sys.argv[2]).convert("RGB").crop((14, 50, 228, 86))
sys.exit(0 if sum(1 for p in ImageChops.difference(a, b).convert("L").getdata() if p > 60) > 40 else 1)
PY
expect_ok "display-key Control_L+a"
expect_ok "display-key BackSpace"
expect_ok "display-scroll 3"
expect_ok "display-move 300 300"
grep -q "DisplayType(4 chars)" "$LOG" && ! grep "AGENT-IPC" "$LOG" | grep -q "dock" && echo "ok: the log says how much was typed, never what" || fail "the log: $(grep 'AGENT-IPC' "$LOG" | grep -i 'type')"
echo "--- refusals"
expect_error "display-click 99999 1" "position"
expect_error "display-click 9000 1" "off the display"
expect_error "display-click -1 2" "position"
expect_error "display-key not a key" "display-key takes"
expect_error "display-key XF86Poweroff" "unknown key"
expect_error "display-type" "needs text"
flood=0; for _ in $(seq 1 30); do case "$(send 'display-move 5 5' 5)" in error:*"slow down"*) flood=1;; esac; done
[ "$flood" = 1 ] && echo "ok: a flood of inputs is slowed down" || fail "no rate limit"
sleep 1.2
echo "--- asking the person for help"
expect_ok "display-help sign in to the portal"
wait_log "ISLAND AgentHelp" 20 && echo "ok: the island says the agent needs help" || fail "no help island: $(grep ISLAND "$LOG" | tail -2)"
expect_error "display-click 10 10" "waiting for the person's help: sign in to the portal"
echo agent-display >> "$STATE/surface-control"
sleep 3
PT="$ROOT/tools/nested/wl-pointer/wl-pointer"
read -r fx fy <<< "$(python3 - "$STATE/layout.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1])); e = d["elements"]["av-done"]
print(f'{(e["x"] + e["w"] / 2) / d["width"]:.4f} {(e["y"] + e["h"] / 2) / d["height"]:.4f}')
PY
)"
niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/help.png"
"$PT" "$fx" "$fy" click
wait_log "AGENT-DISPLAY help done" 20 && echo "ok: \"Done, carry on\" cleared the request" || fail "not cleared"
expect_ok "display-click 10 10"
echo "--- the display is the agent's: nothing of the person's desktop"
mine=$(niri msg --json windows | python3 -c 'import json,sys; print(len([w for w in json.load(sys.stdin) if w["app_id"]=="org.helix.Settings"]))')
[ "$mine" = 0 ] && echo "ok: no window of the agent's appeared on the person's desktop" || fail "$mine windows on the person's desktop"
echo agent-display-stop >> "$STATE/surface-control"; sleep 1
[ "$FAILED" = 0 ] && echo "PASS: agent computer use" || exit 1
