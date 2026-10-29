#!/bin/bash
# helix-agents against a pretend agent in a throwaway home folder: what the box shows it, what flags it is given per profile, and that the launch is logged.
#   tools/agents-test.sh            (needs bubblewrap and a built helix-agents)
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${HELIX_AGENTS_BIN:-$(ls -t "$ROOT/target/fast/helix-agents" "$ROOT/target/release/helix-agents" "$ROOT/target/debug/helix-agents" 2>/dev/null | head -1)}
FAILED=0; fail() { echo "FAIL: $*" >&2; FAILED=1; }
bwrap --version >/dev/null 2>&1 || { echo "skip: no bubblewrap"; exit 0; }
T=$(mktemp -d /tmp/helix-agents-test.XXXXXX); trap 'rm -rf "$T"' EXIT
H=$T/home; P=$H/work/app; mkdir -p "$H/.ssh" "$H/.local/bin" "$H/.claude" "$P" "$T/state" "$T/config" "$T/run"
echo "private key" > "$H/.ssh/id_ed25519"; echo "secret note" > "$H/notes.txt"
# The pretend agent reports what it can see into the project folder, then exits with the profile's flags echoed.
cat > "$H/.local/bin/claude" <<'AGENT'
#!/bin/sh
out=report.txt
{
  echo "args: $*"
  echo "ssh: $(cat "$HOME/.ssh/id_ed25519" 2>/dev/null || echo hidden)"
  echo "notes: $(cat "$HOME/notes.txt" 2>/dev/null || echo hidden)"
  ( echo x > "$HOME/escape.txt" ) 2>/dev/null && echo "home-write: yes" || echo "home-write: no"
  ( echo x > "$HOME/.claude/state.txt" ) 2>/dev/null && echo "state-write: yes" || echo "state-write: no"
  ( echo x > /usr/escape.txt ) 2>/dev/null && echo "usr-write: yes" || echo "usr-write: no"
  echo "dbus: ${DBUS_SESSION_BUS_ADDRESS:-none}"
  echo "key: ${ANTHROPIC_API_KEY:-none}"
  echo "other-key: ${OPENAI_API_KEY:-none}"
  echo "pwd: $(pwd)"
} > "$out"
AGENT
chmod +x "$H/.local/bin/claude"
run() { env -i HOME="$H" PATH="$H/.local/bin:/usr/bin" TERM=xterm USER=tester XDG_STATE_HOME="$T/state" XDG_CONFIG_HOME="$T/config" XDG_RUNTIME_DIR="$T/run" \
  DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus ANTHROPIC_API_KEY=sk-test OPENAI_API_KEY=sk-other HELIX_MCP_COMMAND=/usr/bin/cat "$BIN" "$@" 2>"$T/stderr"; }
echo "--- ask: boxed, the agent keeps its own prompts"
run run claude --dir "$P" || fail "ask run failed: $(cat "$T/stderr")"
R=$P/report.txt
grep -q "^ssh: hidden" "$R" && grep -q "^notes: hidden" "$R" && echo "ok: the rest of the home folder is not there" || fail "home visible: $(cat "$R")"
grep -q "^usr-write: no" "$R" && echo "ok: the system cannot be written (the home folder is a scratch space that is thrown away)" || fail "writable: $(cat "$R")"
grep -q "^state-write: yes" "$R" && echo "ok: the agent's own state folder is writable" || fail "no state write"
grep -q "^dbus: none" "$R" && grep -q "^other-key: none" "$R" && grep -q "^key: sk-test" "$R" && echo "ok: the session bus is not reachable and only its own key is passed" || fail "env: $(cat "$R")"
grep -q "^pwd: $P" "$R" && echo "ok: it starts in the project" || fail "pwd"
grep -q -- "--mcp-config" "$R" && grep -q "mcp-proxy" "$R" && echo "ok: the Helix desktop tools are put in front of it for this run" || fail "no mcp: $(grep args "$R")"
grep -q -- "--dangerously-skip-permissions" "$R" && fail "ask must keep the agent's prompts" || echo "ok: ask leaves the agent's prompts on"
grep -q -- "--append-system-prompt-file" "$R" && echo "ok: it is told about Helix" || fail "no primer"
[ ! -e "$H/escape.txt" ] && [ ! -e /usr/escape.txt ] && echo "ok: and nothing leaked out" || fail "a file escaped"
echo "--- auto: boxed, prompts off"
rm -f "$R"; run run claude --profile auto --dir "$P" -- --model opus || fail "auto run failed: $(cat "$T/stderr")"
grep -q -- "--dangerously-skip-permissions" "$R" && grep -q -- "--model opus" "$R" && echo "ok: the agent works without asking, with the person's own flags after" || fail "auto args: $(grep args "$R")"
grep -q "^ssh: hidden" "$R" && echo "ok: still boxed" || fail "auto not boxed"
echo "--- open: no box"
rm -f "$R"; run run claude --profile open --dir "$P" || fail "open run failed"
grep -q "^notes: secret note" "$R" && echo "ok: open has no box (the agent sees the home folder as it would anywhere)" || fail "open is boxed: $(cat "$R")"
echo "--- danger needs a time limit"
run run claude --profile danger --dir "$P" && fail "danger ran without --for" || { grep -q "limited time" "$T/stderr" && echo "ok: danger is refused without --for"; }
echo "--- a place that is not a project"
run run claude --dir /usr/lib && fail "a system folder was accepted" || echo "ok: a system folder is refused"
echo "--- the launches are logged"
L=$T/state/helix/agents/launches.jsonl
[ "$(grep -c '"event":"start"' "$L")" -ge 3 ] && grep -q '"profile":"auto"' "$L" && grep -q '"event":"end"' "$L" && echo "ok: every launch and its end are in the log" || fail "log: $(cat "$L")"
echo "--- the Helix desktop tools through the proxy (the real MCP adapter, if it is installed)"
if [ -x "$ROOT/tools/helix-agent/.venv/bin/python" ]; then
cat > "$H/.local/bin/claude" <<'AGENT'
#!/bin/sh
# Reads its own --mcp-config, starts what it names (the proxy), speaks MCP initialize to it and reports the server's name.
python3 - "$@" > report.txt 2>&1 <<'PY'
import json, subprocess, sys
args = sys.argv[1:]
config = json.loads(args[args.index("--mcp-config") + 1])["mcpServers"]["helix"]
p = subprocess.Popen([config["command"]] + config["args"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "t", "version": "1"}}}) + "\n")
p.stdin.flush()
print("reply:", p.stdout.readline().strip()[:300])
p.stdin.close()
PY
AGENT
chmod +x "$H/.local/bin/claude"
# The adapter reaches the desktop service over the real session bus, so this step keeps the real runtime folder.
rm -f "$R"; timeout 60 env -i HOME="$H" PATH="$H/.local/bin:/usr/bin" TERM=xterm USER=tester XDG_STATE_HOME="$T/state" XDG_CONFIG_HOME="$T/config" XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" \
  DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=/run/user/$(id -u)/bus}" HELIX_ROOT="$ROOT" "$BIN" run claude --dir "$P" 2>"$T/stderr"
grep -q '"serverInfo"' "$R" && echo "ok: the agent in the box spoke MCP to the real Helix adapter through the proxy" || fail "no MCP reply: $(cat "$R") $(cat "$T/stderr")"
else echo "skip: the MCP adapter's virtualenv is not installed"; fi
echo "--- defaults and links"
run default opencode auto >/dev/null; [ "$(run default)" = "opencode auto" ] && echo "ok: the default agent and profile are remembered" || fail "defaults"
run default claude danger >/dev/null 2>&1 && fail "danger as a default" || echo "ok: danger cannot be the default"
[ "$FAILED" = 0 ] && echo "PASS: helix-agents" || exit 1
