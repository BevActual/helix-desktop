#!/bin/bash
# Started by run.sh: the real helix-service (told that the Settings binary under test is the one that may manage agents), two applications
# that already hold grants, a little audit history, then Helix Settings itself.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${HELIX_SETTINGS_BIN:-$(ls -t "$ROOT/rust/target/fast/helix-settings" "$ROOT/rust/target/release/helix-settings" 2>/dev/null | head -1)}
export PIPEWIRE_REMOTE=helix-nested-no-audio HELIX_RISK_POLICY="$ROOT/protocols/ipc/msg-risk.toml" HELIX_MANAGER_EXE="$BIN"
mkdir -p "$XDG_STATE_HOME/noctalia/agent" "$XDG_CONFIG_HOME/niri"
cp "$ROOT/tools/nested/fixtures/niri-binds.kdl" "$ROOT/tools/nested/fixtures/niri-extra.kdl" "$XDG_CONFIG_HOME/niri/"
printf '{"clients":{"exe:/opt/agents/helper.py":["observe","act"],"exe:/opt/agents/notes-agent":["observe"]},"version":1}' > "$XDG_STATE_HOME/noctalia/agent/grants.json"
chmod 600 "$XDG_STATE_HOME/noctalia/agent/grants.json"
cat > "$XDG_STATE_HOME/noctalia/agent/audit.jsonl" <<'AUDIT'
{"time":"2026-10-07T10:00:00Z","client":"exe:/opt/agents/helper.py","tool":"volume","args":["set","30"],"tier":"act","decision":"allowed","outcome":"ok","tainted":false}
{"time":"2026-10-07T10:00:05Z","client":"exe:/opt/agents/notes-agent","tool":"app-launch","args":["Browser"],"tier":"confirm","decision":"denied","outcome":"the user declined","tainted":true}
AUDIT
chmod 600 "$XDG_STATE_HOME/noctalia/agent/audit.jsonl"
mkdir -p "$HOME/Pictures/Wallpapers"
python3 - "$HOME/Pictures/Wallpapers" <<'PY'
import sys, zlib, struct
def png(path, rgb, w=320, h=200):
    raw = zlib.compress((b"\x00" + bytes(rgb) * w) * h, 6)
    def chunk(tag, data): c = struct.pack(">I", len(data)) + tag + data; return c + struct.pack(">I", zlib.crc32(tag + data))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", raw) + chunk(b"IEND", b""))
png(sys.argv[1] + "/alpha-red.png", (200, 40, 40)); png(sys.argv[1] + "/beta-green.png", (40, 160, 70)); png(sys.argv[1] + "/gamma-blue.png", (50, 80, 210))
PY
"$ROOT/build-debug/helix-service" > "$XDG_STATE_HOME/helix-service.log" 2>&1 &
echo $! > "$XDG_STATE_HOME/helix-service.pid"
# With a desktop registered the service offers the whole catalog (without one only the portable settings), so every Settings page has its rows.
# Wait for the name to be OWNED (asking the service itself would let D-Bus activate the installed one, with the real home folder).
for _ in $(seq 1 40); do gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.NameHasOwner org.helix.Desktop 2>/dev/null | grep -q true && break; sleep 0.25; done
python3 "$ROOT/tools/nested/register-desktop.py" > "$XDG_STATE_HOME/register-desktop.log" 2>&1 &
export HELIX_UI_LAYOUT_LOG="$XDG_STATE_HOME/layout.json"
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$BIN"
