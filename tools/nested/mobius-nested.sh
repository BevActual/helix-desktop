#!/bin/bash
# Started by run.sh: the Rust helix-inference with its scripted (fake) backend on the private bus, then Mobius (AI hub).
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
INFERENCE=${HELIX_INFERENCE_BIN:-$(ls -t "$ROOT/target/fast/helix-inference" "$ROOT/target/release/helix-inference" "$ROOT/target/debug/helix-inference" 2>/dev/null | head -1)}   # the Rust service, run with its scripted backend
BIN=${HELIX_MOBIUS_BIN:-$(ls -t "$ROOT/target/fast/helix-mobius" "$ROOT/target/release/helix-mobius" 2>/dev/null | head -1)}
export HELIX_UI_LAYOUT_LOG="$XDG_STATE_HOME/layout.json"
# A recording to play as the microphone (HELIX_NESTED_SEED_AUDIO=1): one second of a tone, raw 16 kHz 16-bit mono.
if [ -n "${HELIX_NESTED_SEED_AUDIO:-}" ]; then
  python3 - "$XDG_STATE_HOME/mic.raw" <<'PY'
import math, struct, sys
open(sys.argv[1], "wb").write(b"".join(struct.pack("<h", int(8000 * math.sin(i / 20))) for i in range(16000)))
PY
  export HELIX_DICTATE_SOURCE_FILE="$XDG_STATE_HOME/mic.raw"
fi
# A saved chat with a picture in it (HELIX_NESTED_SEED_PICTURE=1): lets a test open it, see the picture, and send it again by editing.
if [ -n "${HELIX_NESTED_SEED_PICTURE:-}" ]; then
python3 - "$XDG_DATA_HOME" <<'PY'
import json, os, struct, sys, zlib
base = os.path.join(sys.argv[1], "helix", "mobius")
os.makedirs(os.path.join(base, "chats"), exist_ok=True)
os.makedirs(os.path.join(base, "images"), exist_ok=True)
def chunk(kind, data): return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
w, h = 40, 30
raw = b"".join(b"\x00" + bytes((200, 40, 40, 255)) * w for _ in range(h))
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
open(os.path.join(base, "images", "00000000000000a1"), "wb").write(png)
chat = {"version": 1, "id": "00000000000000c1", "title": "A picture", "updated": 1760000000, "pinned": False, "project": "", "style": "",
        "messages": [{"role": "user", "text": "What colour is this?", "images": ["00000000000000a1"]},
                     {"role": "assistant", "text": "It looks red.", "images": []}]}
json.dump(chat, open(os.path.join(base, "chats", "00000000000000c1.json"), "w"))
PY
fi
HELIX_INFERENCE_BACKEND=fake HELIX_FAKE_REPLY="${HELIX_FAKE_REPLY:-Sure. Here is how it works:

- first, a **scheduler** wakes up
- then it runs \`cron\` jobs

\`\`\`sh
crontab -e
\`\`\`}" HELIX_FAKE_DELAY_MS="${HELIX_FAKE_DELAY_MS:-250}" "$INFERENCE" > "$XDG_STATE_HOME/helix-inference.log" 2>&1 &
echo $! > "$XDG_STATE_HOME/helix-service.pid"
sleep 1
# Chats keep the title made from their first message in tests, unless one asks for the model to name them (HELIX_NESTED_AUTO_TITLE=1).
[ -n "${HELIX_NESTED_AUTO_TITLE:-}" ] || export HELIX_MOBIUS_NO_AUTO_TITLE=1
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$BIN"
