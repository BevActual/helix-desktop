#!/bin/bash
# Started by run.sh: the model service with its scripted backend on the private bus, a pretend OpenAI-compatible server on this computer (what the
# service's Probe asks), then Helix Models.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
INFERENCE=${HELIX_INFERENCE_BIN:-$(ls -t "$ROOT/target/fast/helix-inference" "$ROOT/target/release/helix-inference" "$ROOT/target/debug/helix-inference" 2>/dev/null | head -1)}
BIN=${HELIX_MODELS_BIN:-$(ls -t "$ROOT/target/fast/helix-models" "$ROOT/target/release/helix-models" 2>/dev/null | head -1)}
export HELIX_UI_LAYOUT_LOG="$XDG_STATE_HOME/layout.json"
mkdir -p "$XDG_STATE_HOME/helix"
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
echo "$PORT" > "$XDG_STATE_HOME/stub.port"
cat > "$XDG_STATE_HOME/stub-openai.py" <<'PY'
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        ok = self.headers.get("Authorization") == "Bearer sk-nested"
        body = json.dumps({"data": [{"id": "big-model"}, {"id": "small-model"}]} if ok else {"error": {"message": "key needed"}}).encode()
        self.send_response(200 if ok else 401); self.send_header("Content-Type", "application/json"); self.end_headers(); self.wfile.write(body)
HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
PY
python3 "$XDG_STATE_HOME/stub-openai.py" "$PORT" > /dev/null 2>&1 &
echo $! > "$XDG_STATE_HOME/stub.pid"
HELIX_INFERENCE_BACKEND=fake HELIX_ENGINE_SANDBOX=off "$INFERENCE" > "$XDG_STATE_HOME/helix-inference.log" 2>&1 &
echo $! > "$XDG_STATE_HOME/inference.pid"
for _ in $(seq 1 60); do busctl --user status org.helix.Inference >/dev/null 2>&1 && break; sleep 0.25; done
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$BIN"
