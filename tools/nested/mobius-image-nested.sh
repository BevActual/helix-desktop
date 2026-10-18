#!/bin/bash
# Mobius with a picture the system picker will hand over (a fake zenity that prints the path of a small PNG).
mkdir -p "$XDG_STATE_HOME/pick-bin"
python3 - "$XDG_STATE_HOME/photo.png" <<'PY'
import struct, sys, zlib
def chunk(k, d): return struct.pack(">I", len(d)) + k + d + struct.pack(">I", zlib.crc32(k + d) & 0xFFFFFFFF)
w, h = 40, 30
raw = b"".join(b"\x00" + b"".join(bytes((200, 40, 40, 255)) if x < 20 else bytes((40, 40, 200, 255)) for x in range(w)) for _ in range(h))
open(sys.argv[1], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
PY
cat > "$XDG_STATE_HOME/pick-bin/zenity" <<PICK
#!/bin/bash
echo "$XDG_STATE_HOME/photo.png"
PICK
chmod +x "$XDG_STATE_HOME/pick-bin/zenity"
export PATH="$XDG_STATE_HOME/pick-bin:$PATH"
exec "$(dirname "$0")/mobius-nested.sh"
