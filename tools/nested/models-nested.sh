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
# A pretend Hugging Face for downloads: one repository, test/Tiny-GGUF, with two versions and a picture projector. The Q8_0 file trickles so it
# can be stopped part way.
HUB_PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
cat > "$XDG_STATE_HOME/stub-hub.py" <<'PY'
import hashlib, json, sys, time
from http.server import BaseHTTPRequestHandler, HTTPServer
blobs = {"tiny-Q4_K_M.gguf": b"GGUF" + bytes(range(256)) * 4000, "tiny-Q8_0.gguf": b"GGUF" + b"8" * 4000000, "mmproj-tiny-f16.gguf": b"GGUF" + b"p" * 2000}
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path.startswith("/api/models/test/Tiny-GGUF/tree/main"):
            body = json.dumps([{"type": "file", "path": n, "size": len(d), "lfs": {"oid": hashlib.sha256(d).hexdigest(), "size": len(d)}} for n, d in blobs.items()]).encode()
            self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body); return
        name = self.path.rsplit("/", 1)[-1]
        data = blobs.get(name)
        if data is None or "/test/Tiny-GGUF/resolve/main/" not in self.path:
            self.send_response(404); self.end_headers(); return
        start = int(self.headers["Range"].split("=")[1].split("-")[0]) if self.headers.get("Range") else 0
        part = data[start:]
        self.send_response(206 if start else 200); self.send_header("Content-Length", str(len(part))); self.end_headers()
        for i in range(0, len(part), 65536):
            self.wfile.write(part[i:i+65536]); time.sleep(0.25 if "Q8" in name else 0.02)
HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
PY
python3 "$XDG_STATE_HOME/stub-hub.py" "$HUB_PORT" > /dev/null 2>&1 &
echo $! > "$XDG_STATE_HOME/hub.pid"
HELIX_MODEL_HUB="http://127.0.0.1:$HUB_PORT" HELIX_INFERENCE_BACKEND=fake HELIX_ENGINE_SANDBOX=off "$INFERENCE" > "$XDG_STATE_HOME/helix-inference.log" 2>&1 &
echo $! > "$XDG_STATE_HOME/inference.pid"
for _ in $(seq 1 60); do busctl --user status org.helix.Inference >/dev/null 2>&1 && break; sleep 0.25; done
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$BIN"
