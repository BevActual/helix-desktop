#!/bin/bash
# Mobius with chat routed to a pretend OpenRouter on this computer: it records each request it gets and answers as a fallback model would,
# with a cost. The routing options for chat are seeded (price preference, one fallback, private providers only).
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$XDG_STATE_HOME/helix/secrets" "$XDG_DATA_HOME/helix/mobius"
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
cat > "$XDG_STATE_HOME/fake-openrouter.py" <<'PY'
import json, os, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
out = sys.argv[2]
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        body = json.dumps({"data": [{"id": "acme/primary"}, {"id": "acme/backup"}]}).encode()
        self.send_response(200); self.send_header("Content-Type", "application/json"); self.end_headers(); self.wfile.write(body)
    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        with open(out, "a") as f: f.write(raw.decode() + "\n")
        body = json.dumps({"model": "acme/backup", "choices": [{"message": {"content": "Answered by the fallback."}}],
                           "usage": {"completion_tokens": 7, "cost": 0.0012}}).encode()
        self.send_response(200); self.send_header("Content-Type", "application/json"); self.end_headers(); self.wfile.write(body)
HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
PY
python3 "$XDG_STATE_HOME/fake-openrouter.py" "$PORT" "$XDG_STATE_HOME/or-requests.jsonl" &
echo $! > "$XDG_STATE_HOME/fake-or.pid"
cat > "$XDG_STATE_HOME/helix/inference-routing.toml" <<TOML
[[backends]]
id = "openrouter"
kind = "openai"
base_url = "http://127.0.0.1:$PORT/v1"
label = "OpenRouter"
api_key_ref = "openrouter"

[profiles.chat]
backend = "openrouter"
model = "acme/primary"
TOML
printf 'sk-or-test-not-real' > "$XDG_STATE_HOME/helix/secrets/backend-openrouter"; chmod 600 "$XDG_STATE_HOME/helix/secrets/backend-openrouter"
echo '["acme/backup","acme/other"]' > "$XDG_DATA_HOME/helix/mobius/cloud-pins.json"
echo '{"chat":{"prefer":"price","fallbacks":["acme/backup"],"private_only":true}}' > "$XDG_DATA_HOME/helix/mobius/cloud-routes.json"
exec "$ROOT/tools/nested/mobius-nested.sh"
