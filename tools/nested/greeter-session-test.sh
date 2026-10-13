#!/bin/bash
# The greeter session script: it starts a private compositor with the greeter in it, and the greeter reaches greetd.
#   tools/nested/ui-suite.sh greeter-session
SCRATCH=$1; ROOT=$(cd "$(dirname "$0")/../.." && pwd); FAILED=0
fail() { echo "FAIL: $*" >&2; FAILED=1; }
LOG=$SCRATCH/state/greetd-log.jsonl
for _ in $(seq 1 60); do grep -q create_session "$LOG" 2>/dev/null && break; sleep 0.5; done
grep -q create_session "$LOG" && echo "ok: the greeter started inside its compositor and reached greetd" || { fail "greeter never reached greetd"; tail -5 "$SCRATCH/state/greeter-session.log"; }
niri msg action screenshot-screen >/dev/null 2>&1; sleep 1.5; ls -t "$SCRATCH"/shots/*.png | head -1 | xargs -I{} cp {} "$SCRATCH/greeter-session.png"
echo "--- fallback: a missing greeter hands over to the text login"
OUT=$(printf '#!/bin/sh\necho "FALLBACK $*"\n' > "$SCRATCH/fake-agreety" && chmod +x "$SCRATCH/fake-agreety" \
  && HELIX_GREETER_BIN=/nonexistent HELIX_GREETER_FALLBACK="$SCRATCH/fake-agreety" HELIX_GREETER_SESSION_CMD=/bin/true \
     HELIX_GREETER_NESTED=1 "$ROOT/greeter/session/helix-greeter-session" 2>/dev/null)
[ "$OUT" = "FALLBACK --cmd /bin/true" ] && echo "ok: fell back to the text login" || fail "no fallback: $OUT"
OUT=$(HELIX_GREETER_BIN=/bin/false HELIX_GREETER_NIRI=/bin/false HELIX_GREETER_FALLBACK="$SCRATCH/fake-agreety" HELIX_GREETER_SESSION_CMD=/bin/true \
     HELIX_GREETER_NESTED=1 "$ROOT/greeter/session/helix-greeter-session" 2>/dev/null)
[ "$OUT" = "FALLBACK --cmd /bin/true" ] && echo "ok: a greeter that fails also falls back" || fail "no fallback after failure: $OUT"
[ $FAILED = 0 ] && echo "ALL ok" || echo FAILED
