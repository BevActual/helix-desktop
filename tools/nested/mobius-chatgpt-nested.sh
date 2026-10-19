#!/bin/bash
# Mobius with a pretend OpenAI on this computer: a sign-in server (discovery, authorize, token with a PKCE check, signing keys, revoke) and the
# API (models, responses). The browser is a script that follows the authorize address, which redirects back to Mobius's local callback.
# Nothing touches a real account.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
D=$XDG_STATE_HOME/fake-openai; mkdir -p "$D" "$XDG_STATE_HOME/open-bin"
openssl genrsa -out "$D/key.pem" 2048 2>/dev/null
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
cat > "$D/server.py" <<'PY'
import base64, hashlib, json, subprocess, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse
port, d = int(sys.argv[1]), sys.argv[2]
ISS = f"http://127.0.0.1:{port}"
b64 = lambda b: base64.urlsafe_b64encode(b).rstrip(b"=").decode()
mod = subprocess.check_output(["openssl", "rsa", "-in", f"{d}/key.pem", "-noout", "-modulus"]).decode().strip().split("=")[1]
N = b64(bytes.fromhex(mod)); E = b64((65537).to_bytes(3, "big"))
state = {"challenge": "", "nonce": "", "tokens": 0}
def log(kind, data):
    with open(f"{d}/log.jsonl", "a") as f: f.write(json.dumps({"kind": kind, **data}) + "\n")
def id_token(aud):
    head = b64(json.dumps({"alg": "RS256", "kid": "k1", "typ": "JWT"}).encode())
    now = int(time.time())
    body = b64(json.dumps({"iss": ISS, "aud": aud, "sub": "user-123", "email": "test@example.com", "name": "Test Person", "nonce": state["nonce"], "iat": now, "exp": now + 3600}).encode())
    sig = subprocess.run(["openssl", "dgst", "-sha256", "-sign", f"{d}/key.pem"], input=f"{head}.{body}".encode(), capture_output=True).stdout
    return f"{head}.{body}.{b64(sig)}"
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, obj=None, headers=None):
        body = json.dumps(obj).encode() if obj is not None else b""
        self.send_response(code)
        for k, v in (headers or {}).items(): self.send_header(k, v)
        if obj is not None: self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        u = urlparse(self.path); q = parse_qs(u.query)
        if u.path == "/.well-known/openid-configuration":
            return self.send(200, {"issuer": ISS, "authorization_endpoint": f"{ISS}/authorize", "token_endpoint": f"{ISS}/token", "jwks_uri": f"{ISS}/jwks", "revocation_endpoint": f"{ISS}/revoke"})
        if u.path == "/authorize":
            log("authorize", {k: v[0] for k, v in q.items()})
            state["challenge"] = q["code_challenge"][0]; state["nonce"] = q["nonce"][0]
            loc = f'{q["redirect_uri"][0]}?code=THECODE&state={q["state"][0]}&client_id=client_test'
            return self.send(302, None, {"Location": loc})
        if u.path == "/jwks": return self.send(200, {"keys": [{"kty": "RSA", "kid": "k1", "alg": "RS256", "n": N, "e": E}]})
        if u.path == "/v1/models":
            log("models", {"auth": self.headers.get("Authorization")})
            return self.send(200, {"data": [{"id": "gpt-5"}, {"id": "text-embedding-3-small"}, {"id": "o4-mini"}, {"id": "whisper-1"}]})
        self.send(404, {"error": "nope"})
    def do_POST(self):
        u = urlparse(self.path); raw = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
        if u.path == "/token":
            f = {k: v[0] for k, v in parse_qs(raw).items()}; log("token", {k: v for k, v in f.items() if k != "code_verifier"})
            if f.get("grant_type") == "authorization_code":
                ok = f.get("code") == "THECODE" and f.get("client_id") == "client_test" and b64(hashlib.sha256(f["code_verifier"].encode()).digest()) == state["challenge"]
                if not ok: return self.send(400, {"error": "invalid_grant"})
                state["tokens"] += 1
                return self.send(200, {"access_token": f"at-{state['tokens']}", "refresh_token": "rt-1", "expires_in": 3600, "scope": "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct", "id_token": id_token("client_test")})
            if f.get("grant_type") == "refresh_token" and f.get("refresh_token") == "rt-1":
                state["tokens"] += 1
                return self.send(200, {"access_token": f"at-{state['tokens']}", "refresh_token": "rt-1", "expires_in": 3600, "scope": "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct"})
            return self.send(400, {"error": "invalid_grant"})
        if u.path == "/revoke": log("revoke", {"body": raw}); return self.send(200, {})
        if u.path == "/v1/responses":
            body = json.loads(raw); log("responses", {"auth": self.headers.get("Authorization"), "body": body})
            return self.send(200, {"model": "gpt-5-2026", "output": [{"type": "message", "content": [{"type": "output_text", "text": "Answered with the plan."}]}], "usage": {"output_tokens": 6}})
        self.send(404, {"error": "nope"})
ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
PY
python3 "$D/server.py" "$PORT" "$D" &
echo $! > "$XDG_STATE_HOME/fake-openai.pid"
# the "browser": follow the authorize address, which redirects back to Mobius's local callback
cat > "$XDG_STATE_HOME/open-bin/xdg-open" <<'OPEN'
#!/bin/bash
echo "$1" >> "$XDG_STATE_HOME/opened-urls.txt"
curl -s -L --max-redirs 3 -o /dev/null "$1"
OPEN
chmod +x "$XDG_STATE_HOME/open-bin/xdg-open"
export PATH="$XDG_STATE_HOME/open-bin:$PATH" HELIX_CHATGPT_ISSUER="http://127.0.0.1:$PORT" HELIX_CHATGPT_API="http://127.0.0.1:$PORT/v1"
sleep 1
exec "$ROOT/tools/nested/mobius-nested.sh"
