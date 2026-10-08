#!/bin/bash
# Started by run.sh inside the private session: writes a test image and a settings file for it, then runs the wallpaper process.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
python3 - "${XDG_STATE_HOME}/wp.png" <<'PY'
import sys, zlib, struct
W, H = 4000, 2000
rows = []
for y in range(H):
    row = bytearray([0])
    for x in range(W):
        if abs(x - W // 2) < 150 and abs(y - H // 2) < 150: row += bytes((0, 200, 0))
        elif x < W // 2: row += bytes((255, 0, 0))
        else: row += bytes((0, 0, 255))
    rows.append(bytes(row))
raw = zlib.compress(b"".join(rows), 6)
def chunk(tag, data): c = struct.pack(">I", len(data)) + tag + data; return c + struct.pack(">I", zlib.crc32(tag + data))
open(sys.argv[1], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)) + chunk(b"IDAT", raw) + chunk(b"IEND", b""))
PY
printf '[wallpaper]\n[wallpaper.default]\npath = "%s/wp.png"\n[wallpaper.monitors.NOPE-1]\npath = "color:#000000"\n' "${XDG_STATE_HOME}" > "${XDG_STATE_HOME}/noctalia-settings.toml"
export HELIX_WALLPAPER_CHOICE="${XDG_STATE_HOME}/choice.toml"
export HELIX_WALLPAPER_SETTINGS="${XDG_STATE_HOME}/noctalia-settings.toml"
exec "$ROOT/rust/target/release/helix-wallpaper"
