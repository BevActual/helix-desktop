#!/bin/bash
# Helix Software's Fedora backend against tools/nested/fake-packagekit.py on a private "system" bus, with a fixture AppStream catalog and no Flatpak.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${HELIX_SOFTWARE_BIN:-$(ls -t "$ROOT/target/fast/helix-software" "$ROOT/target/release/helix-software" 2>/dev/null | head -1)}
export HELIX_UI_LAYOUT_LOG="$XDG_STATE_HOME/layout.json"
# A bus of its own, standing in for the system bus (the real PackageKit is never touched).
BUS=$(dbus-daemon --config-file="$ROOT/tools/nested/bus.conf" --fork --print-address --print-pid 2>/dev/null)
ADDRESS=$(echo "$BUS" | sed -n 1p)
[ -n "$ADDRESS" ] || { echo "no private bus"; exit 1; }
echo "$BUS" | sed -n 2p > "$XDG_STATE_HOME/private-bus.pid"
python3 "$ROOT/tools/nested/fake-packagekit.py" "$XDG_STATE_HOME" "$ADDRESS" > "$XDG_STATE_HOME/fake-packagekit.out" 2>&1 &
echo $! > "$XDG_STATE_HOME/helix-service.pid"
for _ in $(seq 1 50); do grep -q ready "$XDG_STATE_HOME/fake-packagekit.out" 2>/dev/null && break; sleep 0.1; done
# The fixture catalog gets its icons made here (small flat PNGs), so none are committed.
CATALOG=$XDG_STATE_HOME/rpm-catalog
mkdir -p "$CATALOG"; cp -r "$ROOT/tools/nested/software-rpm-catalog/xml" "$CATALOG/"
python3 - "$CATALOG" <<'PY'
import os, struct, sys, zlib
def png(color):
    raw = b"".join(b"\x00" + bytes(color) * 64 for _ in range(64))
    def chunk(tag, data): return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 64, 64, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
base = os.path.join(sys.argv[1], "icons", "fixture", "64x64"); os.makedirs(base)
for name, color in (("calc", (60, 120, 220)), ("writer", (220, 120, 60)), ("locked", (120, 120, 120))):
    open(os.path.join(base, name + ".png"), "wb").write(png(color))
PY
export DBUS_SYSTEM_BUS_ADDRESS="$ADDRESS"
export HELIX_SOFTWARE_RPM_CATALOG="$CATALOG"
export HELIX_SOFTWARE_NO_FLATPAK=1
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$BIN"
