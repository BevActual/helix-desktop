#!/bin/bash
# Settings → Providers → ChatGPT plan, against a pretend OpenAI: Continue with ChatGPT signs in (PKCE, a dynamic client, a signed identity), the
# tokens are kept only in the secret store, the plan's chat models are offered for a job, a message goes to the Responses API with the plan's
# token, and an expired token is refreshed first. Nothing touches a real account.
#   tools/nested/ui-suite.sh mobius-chatgpt
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
source "$ROOT/tools/nested/ui-lib.sh"
fail() { echo "FAIL: $*" >&2; FAILED=1; }
shot() { niri msg action screenshot-screen --show-pointer false >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/$1.png"; }
wait_id() { for _ in $(seq 1 ${2:-40}); do has_id "$1" && return 0; sleep 0.25; done; return 1; }
for _ in $(seq 1 80); do [ -f "$SCRATCH/state/layout.json" ] && has_id open-models && break; sleep 0.25; done; sleep 1
loc=$(python3 - <<'PY'
import json, subprocess
w = next(x for x in json.loads(subprocess.check_output(["niri","msg","--json","windows"])) if "mobius" in (x["app_id"] or "").lower())
o = next(iter(json.loads(subprocess.check_output(["niri","msg","--json","outputs"])).values()))["logical"]
l = w["layout"]; t = l.get("tile_pos_in_workspace_view") or [0,0]; f = l.get("window_offset_in_tile") or [0,0]
print(f'{t[0]+f[0]} {t[1]+f[1]}|{o["width"]} {o["height"]}')
PY
)
export UI_ORIGIN=${loc%%|*} UI_SCREEN=${loc##*|} UI_REVEAL="760 150"
LOG=$SCRATCH/state/fake-openai/log.jsonl
SECRET=$SCRATCH/state/helix/secrets/backend-chatgpt-plan
echo "--- 1. sign in"
click_id open-models; sleep 1; wait_id models-nav-providers && click_id models-nav-providers; sleep 1
wait_id chatgpt-signin && echo "ok: Providers offers Continue with ChatGPT" || fail "no sign-in button"
shot chatgpt-signin; shot chatgpt-signin
click_id chatgpt-signin
wait_id chatgpt-account 60 && echo "ok: the sign-in finished and the account is shown" || { fail "never connected"; shot chatgpt-fail; shot chatgpt-fail; }
python3 - "$LOG" <<'PY' && echo "ok: the browser was sent a registration request with PKCE, a host id and the plan scope" || fail "authorize request not as expected"
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1])]
a = next(r for r in rows if r["kind"] == "authorize")
assert a["client_id"] == "dynamic_agent_client" and a["agent_name_hint"] == "Helix Mobius", a
assert a["code_challenge_method"] == "S256" and a["response_type"] == "code", a
assert a["ext_agent_host_id"].startswith("urn:uuid:"), a
assert a["resource"] == "https://api.openai.com/v1" and "chatgpt.tokens.use.direct" in a["scope"] and "offline_access" in a["scope"], a
assert a["redirect_uri"].startswith("http://127.0.0.1:") and a["redirect_uri"].endswith("/callback"), a
t = next(r for r in rows if r["kind"] == "token")
assert t["grant_type"] == "authorization_code" and t["client_id"] == "client_test", t
PY
shot chatgpt-connected; shot chatgpt-connected
echo "--- 2. what is kept, and where"
python3 - "$SECRET" "$SCRATCH" <<'PY' && echo "ok: the tokens are only in the secret store; the profile and routing hold none" || fail "token storage not as expected"
import glob, json, os, stat, sys
secret, scratch = sys.argv[1], sys.argv[2]
assert stat.S_IMODE(os.stat(secret).st_mode) == 0o600
c = json.load(open(secret)); assert c["access_token"] == "at-1" and c["refresh_token"] == "rt-1", c
for path in glob.glob(scratch + "/**/*", recursive=True):
    if os.path.isfile(path) and path != secret and "/fake-openai/" not in path and not path.endswith((".png", ".log")) and os.path.getsize(path) < 2_000_000:
        text = open(path, errors="ignore").read()
        assert "rt-1" not in text and '"at-1"' not in text, path
p = json.load(open(glob.glob(scratch + "/data/helix/mobius/chatgpt-profile.json")[0]))
assert p["email"] == "test@example.com" and p["client_id"] == "client_test", p
assert "chatgpt" in open(glob.glob(scratch + "/state/helix/inference-routing.toml")[0]).read()
PY
echo "--- 3. use it for a job"
click_id models-nav-models; sleep 1; click_id job-pick-chat; sleep 0.8
wait_id job-opt-chat-chatgpt-gpt-5 && echo "ok: the plan's chat models are offered" || fail "no ChatGPT models in the menu"
has_id job-opt-chat-chatgpt-text-embedding-3-small && fail "an embedding model was offered for chat" || echo "ok: models that cannot chat are not"
shot chatgpt-picker; shot chatgpt-picker
click_id job-opt-chat-chatgpt-gpt-5; sleep 1.5
echo "--- 4. a message uses the plan"
click_id models-back; sleep 1.2
click_id input; wtype "Hello there"; wtype -k Return
for _ in $(seq 1 40); do grep -q '"responses"' "$LOG" 2>/dev/null && break; sleep 0.5; done
python3 - "$LOG" <<'PY' && echo "ok: the request went to the Responses API with the plan's token and nothing stored" || fail "responses request not as expected"
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1])]
r = [r for r in rows if r["kind"] == "responses"][0]
assert r["auth"] == "Bearer at-1", r["auth"]
b = r["body"]; assert b["model"] == "gpt-5" and b["store"] is False and b["stream"] is False, b
assert any(m["role"] == "user" and "Hello there" in m["content"] for m in b["input"]), b
PY
for _ in $(seq 1 40); do has_id stats-1 && break; sleep 0.5; done
F=$(ls -t "$SCRATCH"/data/helix/mobius/chats/*.json | head -1)
python3 - "$F" <<'PY' && echo "ok: the reply names the model and has no cost (the plan pays)" || fail "reply stats not as expected"
import json, sys
m = json.load(open(sys.argv[1]))["messages"][-1]
assert m["text"] == "Answered with the plan." and m["stats"]["model"] == "gpt-5-2026" and m["stats"]["tokens"] == 6 and m["stats"]["cost_micro"] is None, m
PY
shot chatgpt-reply; shot chatgpt-reply
echo "--- 5. an expired token is refreshed"
python3 - "$SECRET" <<'PY'
import json, sys
c = json.load(open(sys.argv[1])); c["expires_at"] = 0; json.dump(c, open(sys.argv[1], "w"))
PY
click_id input; wtype "Again please"; wtype -k Return
for _ in $(seq 1 40); do [ "$(grep -c '"responses"' "$LOG")" -ge 2 ] && break; sleep 0.5; done
python3 - "$LOG" <<'PY' && echo "ok: the token was refreshed first, then used" || fail "refresh not as expected"
import json, sys
rows = [json.loads(l) for l in open(sys.argv[1])]
refresh = [r for r in rows if r["kind"] == "token" and r.get("grant_type") == "refresh_token"]
assert refresh and refresh[0]["client_id"] == "client_test" and refresh[0]["resource"] == "https://api.openai.com/v1", refresh
last = [r for r in rows if r["kind"] == "responses"][-1]
assert last["auth"] == "Bearer at-2", last["auth"]
PY
[ $FAILED = 0 ] && echo "PASS: mobius chatgpt" || exit 1
