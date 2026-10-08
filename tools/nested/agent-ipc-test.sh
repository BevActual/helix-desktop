#!/bin/bash
# The shell's command socket, spoken to exactly as the desktop service speaks to a shell: one line in (`/\x1e<command> <arguments>`), one reply out.
# Checks what each command did (the settings the fake desktop received, the shell's own log) and, as important, what must be refused.
#   HELIX_NESTED_SCRIPT=tools/nested/agent-ipc-test.sh tools/nested/run.sh agent-ipc tools/nested/tauri-fake-desktop.sh
set -u
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); LOG=$SCRATCH/program.log; FAILED=0
STATE=$SCRATCH/state; SETTINGS=$STATE/fake-settings-actions.log
fail() { echo "FAIL: $*" >&2; FAILED=1; }
for _ in $(seq 1 120); do grep -q "AGENT-IPC serving" "$LOG" 2>/dev/null && break; sleep 0.25; done
SOCK=$(ls "$STATE"/noctalia-*.sock 2>/dev/null | head -1); TOOLS=$(ls "$STATE"/helix-shell-tools-* 2>/dev/null | head -1)
[ -S "$SOCK" ] && echo "ok: the shell serves $(basename "$SOCK")" || { fail "no socket"; exit 1; }
sleep 2
send() { python3 - "$SOCK" "$1" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX); s.settimeout(5); s.connect(sys.argv[1])
s.sendall(("/\x1e" + sys.argv[2]).encode()); s.shutdown(socket.SHUT_WR)
data = b""
while True:
    chunk = s.recv(4096)
    if not chunk: break
    data += chunk
print(data.decode().strip())
PY
}
expect_reply() { local got; got=$(send "$1"); [ "$got" = "$2" ] && echo "ok: '$1' -> $2" || fail "'$1' gave '$got', wanted '$2'"; }
expect_error() { local got; got=$(send "$1"); case "$got" in error:*) echo "ok: '$1' refused ($got)";; *) fail "'$1' was not refused: '$got'";; esac; }
echo "--- 1. the socket and the tool list are private and say what is supported"
[ "$(stat -c %a "$SOCK")" = 600 ] && echo "ok: the socket is owner only" || fail "socket mode $(stat -c %a "$SOCK")"
[ "$(head -1 "$TOOLS")" = "$(cat "$STATE/shell.pid")" ] || [ -n "$(head -1 "$TOOLS")" ] && echo "ok: the tool list starts with the shell's pid" || fail "no pid line"
grep -qx "theme-mode-set" "$TOOLS" && grep -qx "volume-up" "$TOOLS" && ! grep -qx "bar-toggle" "$TOOLS" && echo "ok: it lists supported commands only" || fail "tool list wrong: $(tr '\n' ' ' < "$TOOLS")"
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
echo "--- 3. what must be refused"
for bad in "bar-toggle" "session" "wallpaper-next" "clipboard-copy secret" "volume-up 500" "volume-up -5" "volume-set" "theme-mode-set sepia" "panel-open settings" "panel-open launcher extra" "notification-dnd-set maybe" "volume-up;touch x" ""; do expect_error "$bad"; done
n=$(grep -c "UPDATE" "$SETTINGS"); sleep 0.3
expect_error "theme-mode-set sepia" >/dev/null; [ "$(grep -c UPDATE "$SETTINGS")" = "$n" ] && echo "ok: a refused command changed nothing" || fail "a refused command changed a setting"
echo "--- 4. nothing about the arguments reaches the log"
send "volume-set 37" >/dev/null; grep -q "secret" "$LOG" && fail "an argument was logged" || echo "ok: no stray text in the log"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
