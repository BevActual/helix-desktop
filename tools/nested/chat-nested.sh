#!/bin/bash
# Started by run.sh: the real helix-inference with its scripted (fake) backend on the private bus, then Mobius (AI hub).
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${HELIX_CHAT_BIN:-$(ls -t "$ROOT/target/fast/helix-mobius" "$ROOT/target/fast/helix-chat" "$ROOT/target/release/helix-mobius" "$ROOT/target/release/helix-chat" 2>/dev/null | head -1)}
export HELIX_UI_LAYOUT_LOG="$XDG_STATE_HOME/layout.json"
HELIX_INFERENCE_BACKEND=fake HELIX_FAKE_REPLY="${HELIX_FAKE_REPLY:-Sure. Here is how it works:

- first, a **scheduler** wakes up
- then it runs \`cron\` jobs

\`\`\`sh
crontab -e
\`\`\`}" HELIX_FAKE_DELAY_MS="${HELIX_FAKE_DELAY_MS:-80}" "$ROOT/build-debug/helix-inference" > "$XDG_STATE_HOME/helix-inference.log" 2>&1 &
echo $! > "$XDG_STATE_HOME/helix-service.pid"
sleep 1
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$BIN"
