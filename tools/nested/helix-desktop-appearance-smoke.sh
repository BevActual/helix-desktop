#!/usr/bin/env bash
# Private-bus smoke: Appearance1 palette-previews (local + opt-in catalog fetch).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${HELIX_SERVICE_BIN:-$ROOT/target/debug/helix-desktop}"
export XDG_CONFIG_HOME XDG_STATE_HOME
XDG_CONFIG_HOME="$(mktemp -d)"
XDG_STATE_HOME="$(mktemp -d)"
mkdir -p "$XDG_CONFIG_HOME/helix/palettes" \
  "$XDG_STATE_HOME/helix/community-palettes/.catalog"
cat >"$XDG_STATE_HOME/helix/community-palettes/.catalog/palettes.json" <<'JSON'
{"palettes":[{"name":"Oxocarbon","dark":{"surface":"#161616","primary":"#78a9ff","secondary":"#ee5396","tertiary":"#42be65"},"light":{"surface":"#ffffff","primary":"#0f62fe","secondary":"#da1e28","tertiary":"#198038"}}]}
JSON
cat >"$XDG_CONFIG_HOME/helix/palettes/SmokeCustom.json" <<'JSON'
{"dark":{"mSurface":"#010101","mPrimary":"#abcdef","mSecondary":"#112233","mTertiary":"#445566"},"light":{"surface":"#fefefe","primary":"#010203","secondary":"#040506","tertiary":"#070809"}}
JSON

REMOTE_DIR="$(mktemp -d)"
cat >"$REMOTE_DIR/palettes.json" <<'JSON'
{"palettes":[{"name":"RemoteInk","dark":{"surface":"#0a0a0a","primary":"#ffcc00","secondary":"#00ccff","tertiary":"#cc00ff"},"light":{"surface":"#f5f5f5","primary":"#886600","secondary":"#006688","tertiary":"#660088"}}]}
JSON
PORT_FILE="$(mktemp)"
python3 - "$REMOTE_DIR" "$PORT_FILE" <<'PY' &
import http.server, socketserver, sys
from pathlib import Path
root = Path(sys.argv[1])
port_file = Path(sys.argv[2])
class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=str(root), **k)
    def log_message(self, *a):
        pass
with socketserver.TCPServer(("127.0.0.1", 0), Handler) as httpd:
    port_file.write_text(str(httpd.server_address[1]))
    httpd.serve_forever()
PY
HTTP_PID=$!

"$BIN" &
PID=$!
cleanup() {
  kill "$PID" "$HTTP_PID" 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 40); do
  gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
    --method org.freedesktop.DBus.NameHasOwner org.helix.Desktop 2>/dev/null | grep -q true && break
  sleep 0.05
done

snap="$(gdbus call --session --dest org.helix.Desktop --object-path /org/helix/Desktop \
  --method org.helix.Desktop.Appearance1.GetSnapshot)"
echo "$snap" | grep -q Helix
echo "$snap" | grep -q Oxocarbon
echo "$snap" | grep -q SmokeCustom

cat >"$XDG_CONFIG_HOME/helix/palettes/AfterRefresh.json" <<'JSON'
{"dark":{"surface":"#222222","primary":"#ff00ff","secondary":"#00ffff","tertiary":"#ffff00"},"light":{"surface":"#dddddd","primary":"#110011","secondary":"#001100","tertiary":"#000011"}}
JSON
gdbus call --session --dest org.helix.Desktop --object-path /org/helix/Desktop \
  --method org.helix.Desktop.Appearance1.RefreshPalettes >/dev/null
snap="$(gdbus call --session --dest org.helix.Desktop --object-path /org/helix/Desktop \
  --method org.helix.Desktop.Appearance1.GetSnapshot)"
echo "$snap" | grep -q AfterRefresh

# Restart with catalog URL so RefreshPalettes can fetch
for _ in $(seq 1 40); do
  [[ -s "$PORT_FILE" ]] && break
  sleep 0.05
done
PORT="$(cat "$PORT_FILE")"
kill "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true
export HELIX_COMMUNITY_PALETTE_CATALOG_URL="http://127.0.0.1:${PORT}/palettes.json"
"$BIN" &
PID=$!
for _ in $(seq 1 40); do
  gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
    --method org.freedesktop.DBus.NameHasOwner org.helix.Desktop 2>/dev/null | grep -q true && break
  sleep 0.05
done
gdbus call --session --dest org.helix.Desktop --object-path /org/helix/Desktop \
  --method org.helix.Desktop.Appearance1.RefreshPalettes >/dev/null
snap="$(gdbus call --session --dest org.helix.Desktop --object-path /org/helix/Desktop \
  --method org.helix.Desktop.Appearance1.GetSnapshot)"
echo "$snap" | grep -q RemoteInk

echo "APPEARANCE_SMOKE_OK"
