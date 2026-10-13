#!/bin/bash
# The shell's command socket, spoken to exactly as the desktop service speaks to a shell: one line in (`/\x1e<command> <arguments>`), one reply out.
# Checks what each command did (the settings the fake desktop received, the shell's own log) and, as important, what must be refused.
#   HELIX_NESTED_SCRIPT=tools/nested/agent-ipc-test.sh tools/nested/run.sh agent-ipc tools/nested/tauri-fake-desktop.sh
set -u
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
STATE=$SCRATCH/state; SETTINGS=$STATE/fake-settings-actions.log
fail() { echo "FAIL: $*" >&2; FAILED=1; }
for _ in $(seq 1 120); do grep -q "AGENT-IPC serving" "$LOG" 2>/dev/null && break; sleep 0.25; done
SOCK=$(ls "$STATE"/helix-*.sock 2>/dev/null | head -1); TOOLS=$(ls "$STATE"/helix-shell-tools-* 2>/dev/null | head -1)
[ -S "$SOCK" ] && echo "ok: the shell serves $(basename "$SOCK")" || { fail "no socket"; exit 1; }
sleep 2
send() { python3 - "$SOCK" "$1" "${2:-5}" <<'PY'
import socket, sys
timeout = float(sys.argv[3])
s = socket.socket(socket.AF_UNIX); s.settimeout(timeout); s.connect(sys.argv[1])
s.sendall(("/\x1e" + sys.argv[2]).encode()); s.shutdown(socket.SHUT_WR)
data = b""
while True:
    chunk = s.recv(4096)
    if not chunk: break
    data += chunk
print(data.decode().strip())
PY
}
expect_reply() { local got; got=$(send "$1" "${3:-5}"); [ "$got" = "$2" ] && echo "ok: '$1' -> $2" || fail "'$1' gave '$got', wanted '$2'"; }
expect_error() { local got; got=$(send "$1"); case "$got" in error:*) echo "ok: '$1' refused ($got)";; *) fail "'$1' was not refused: '$got'";; esac; }
echo "--- 1. the socket and the tool list are private and say what is supported"
[ "$(stat -c %a "$SOCK")" = 600 ] && echo "ok: the socket is owner only" || fail "socket mode $(stat -c %a "$SOCK")"
[ "$(head -1 "$TOOLS")" = "$(cat "$STATE/shell.pid")" ] || [ -n "$(head -1 "$TOOLS")" ] && echo "ok: the tool list starts with the shell's pid" || fail "no pid line"
grep -qx "theme-mode-set" "$TOOLS" && grep -qx "volume-up" "$TOOLS" && grep -qx "bar-toggle" "$TOOLS" && echo "ok: it lists supported commands only" || fail "tool list wrong: $(tr '\n' ' ' < "$TOOLS")"
echo "--- 2. commands do what they say"
expect_reply "theme-mode-set light" "ok"; sleep 0.5
grep -q 'UPDATE theme.mode "light"' "$SETTINGS" && echo "ok: the desktop was told to use the light theme" || fail "no light update: $(cat "$SETTINGS")"
expect_reply "theme-mode-set auto" "ok"; sleep 0.5
grep -q 'UPDATE theme.mode "auto"' "$SETTINGS" && echo "ok: automatic theme" || fail "no auto update"
expect_reply "nightlight-enable" "ok"; sleep 0.5
grep -q 'UPDATE nightlight.enabled true' "$SETTINGS" && echo "ok: night light on" || fail "no night light update"
expect_reply "notification-dnd-set on" "ok"; sleep 0.5
expect_reply "notification-dnd-status" "on"
expect_reply "notification-dnd-toggle" "ok"; sleep 0.5
expect_reply "notification-dnd-status" "off"
expect_reply "panel-open launcher" "ok"; sleep 1
grep -q "PANEL open" "$LOG" && echo "ok: the launcher opened" || fail "no panel opened"
expect_reply "panel-close" "ok"; sleep 0.8
expect_reply "bar-hide" "ok"; sleep 0.5
grep -q "BAR hidden" "$LOG" && echo "ok: bar-hide hid the bar" || fail "no BAR hidden log line"
sleep 0.5
expect_reply "bar-show" "ok" 15; sleep 0.5
grep -q "BAR shown" "$LOG" && echo "ok: bar-show restored the bar" || fail "no BAR shown log line"
expect_reply "bar-toggle" "ok"; sleep 0.3
echo "--- 2b. status describes the desktop without anything a program wrote; windows are closed by the ids it lists"
APP_ID=agentwin TARGET_FILE="$STATE/agentwin.txt" python3 "$ROOT/tools/nested/text-target.py" >/dev/null 2>&1 & WIN=$!
for _ in $(seq 1 40); do send "status" | grep -q '"agentwin"' && break; sleep 0.25; done
STATUS=$(send "status"); echo "$STATUS" | cut -c1-300
echo "$STATUS" | python3 -c "
import json, sys
d = json.load(sys.stdin)
assert d['do_not_disturb'] in (True, False) and isinstance(d['workspaces'], list)
w = [x for x in d['windows'] if x['app'] == 'agentwin']
assert w and set(w[0]) == {'id', 'app', 'verified', 'focused'}, d['windows']
assert w[0]['verified'] is False, 'an application that is not installed is not verified'
assert 'title' not in json.dumps(d), 'titles are program text'
print('ok: status lists the window by id and app name only')" || fail "status shape"
ID=$(echo "$STATUS" | python3 -c "import json,sys; print([x['id'] for x in json.load(sys.stdin)['windows'] if x['app']=='agentwin'][0])")
expect_error "window-close 99999999"
expect_reply "workspace-switch next" "ok"
expect_reply "window-close $ID" "ok"; sleep 1.5
niri msg --json windows | python3 -c "import json,sys; sys.exit(1 if any(x['app_id']=='agentwin' for x in json.load(sys.stdin)) else 0)" && echo "ok: the window is gone" || fail "the window is still there"
kill $WIN 2>/dev/null

echo "--- 2c. app launch and clipboard (same paths agents use after Agent Surface Allow)"
grep -qx "app-launch" "$TOOLS" && grep -qx "clipboard-copy" "$TOOLS" && grep -qx "clipboard-text" "$TOOLS" && echo "ok: tool list includes launch and clipboard" || fail "missing launch/clipboard in tool list"
expect_error "app-launch"
expect_error "app-launch no-such-helix-app-zzz"
expect_error "clipboard-copy"
MARKER="helix-agent-ipc-$(date +%s)"
expect_reply "clipboard-copy $MARKER" "ok"
GOT=$(send "clipboard-text")
[ "$GOT" = "$MARKER" ] && echo "ok: clipboard round-trip" || fail "clipboard-text gave '$GOT', wanted '$MARKER'"
expect_reply "clipboard-clear" "ok"
CLEARED=$(send "clipboard-text")
[ -z "$CLEARED" ] && echo "ok: clipboard cleared" || fail "clipboard still held '$CLEARED'"
grep -q "$MARKER" "$LOG" && fail "clipboard text was logged" || echo "ok: clipboard text stayed out of the log"

echo "--- 2d. desktop-capture focused returns a png path"
grep -qx "desktop-capture" "$TOOLS" && echo "ok: tool list includes desktop-capture" || fail "missing desktop-capture in tool list"
CAP=$(send "desktop-capture focused" 30)
case "$CAP" in
  ok*)
    PNG=$(printf '%s' "$CAP" | sed 's/^ok //')
    [ -f "$PNG" ] && echo "ok: desktop-capture wrote a png" || fail "desktop-capture path missing: $PNG"
    ;;
  *) fail "desktop-capture focused: $CAP";;
esac

echo "--- 2d-panel. helix-panel desktop-capture focused"
PANEL_OUT=$("$ROOT/tools/helix-panel" desktop-capture focused 2>&1) || true
case "$PANEL_OUT" in
  ok*)
    PNG=$(printf '%s' "$PANEL_OUT" | sed 's/^ok //')
    [ -f "$PNG" ] && echo "ok: helix-panel desktop-capture wrote a png" || fail "helix-panel path missing: $PNG"
    ;;
  *) fail "helix-panel desktop-capture: $PANEL_OUT";;
esac

if grep -qE 'starting version .* helix' "$SCRATCH/niri.log" 2>/dev/null; then
  echo "--- 2e. helix-niri desktop-capture all stitches outputs"
  CAP=$(send "desktop-capture all" 45)
  case "$CAP" in
    ok*)
      PNG=$(printf '%s' "$CAP" | sed 's/^ok //')
      [ -f "$PNG" ] && echo "ok: desktop-capture all wrote a png" || fail "desktop-capture all path missing: $PNG"
      ;;
    *) fail "desktop-capture all: $CAP";;
  esac
  OUT_NAME=$(grep -oE 'putting output \K\S+' "$SCRATCH/niri.log" 2>/dev/null | head -1)
  if [ -z "$OUT_NAME" ] && [ -n "${NIRI_SOCKET:-}" ]; then
    OUT_NAME=$(niri msg --json outputs 2>/dev/null | python3 -c "import json,sys; d=json.load(sys.stdin); v=next(iter(d.values()), None) if isinstance(d, dict) else (d[0] if d else None); print(v.get('name','') if isinstance(v, dict) else '')" 2>/dev/null || true)
  fi
  if [ -n "$OUT_NAME" ]; then
    echo "--- 2f. helix-niri desktop-capture named output ($OUT_NAME)"
    CAP=$(send "desktop-capture $OUT_NAME" 45)
    case "$CAP" in
      ok*)
        PNG=$(printf '%s' "$CAP" | sed 's/^ok //')
        [ -f "$PNG" ] && echo "ok: desktop-capture output wrote a png" || fail "desktop-capture output path missing: $PNG"
        ;;
      *) fail "desktop-capture $OUT_NAME: $CAP";;
    esac
  else
    echo "skip: no output name from niri msg outputs"
  fi
fi

echo "--- 3. what must be refused"
for bad in "window-close abc" "window-close -1" "workspace-switch up" "status now" "bar-toggle extra" "session" "wallpaper-next" "volume-up 500" "volume-up -5" "volume-set" "theme-mode-set sepia" "panel-open settings" "panel-open launcher extra" "notification-dnd-set maybe" "volume-up;touch x" ""; do expect_error "$bad"; done
n=$(grep -c "UPDATE" "$SETTINGS"); sleep 0.3
expect_error "theme-mode-set sepia" >/dev/null; [ "$(grep -c UPDATE "$SETTINGS")" = "$n" ] && echo "ok: a refused command changed nothing" || fail "a refused command changed a setting"
echo "--- 4. nothing about the arguments reaches the log"
send "volume-set 37" >/dev/null; grep -q "secret" "$LOG" && fail "an argument was logged" || echo "ok: no stray text in the log"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
