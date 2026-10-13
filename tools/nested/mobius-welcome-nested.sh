#!/bin/bash
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$XDG_STATE_HOME/picker-bin"
python3 - "$XDG_STATE_HOME" <<'PY'
import pathlib,struct,zlib,sys
p=pathlib.Path(sys.argv[1]);(p/'welcome-note.txt').write_text('A small text attachment.')
def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
w,h=40,30;raw=b''.join(b'\0'+bytes((20,90,190,255))*w for _ in range(h))
(p/'welcome-picture.png').write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))
PY
cat > "$XDG_STATE_HOME/picker-bin/zenity" <<'PICKER'
#!/bin/bash
if [[ "$*" == *'Add pictures'* ]]; then printf '%s\n' "$XDG_STATE_HOME/welcome-picture.png"; else printf '%s\n' "$XDG_STATE_HOME/welcome-note.txt"; fi
PICKER
chmod +x "$XDG_STATE_HOME/picker-bin/zenity"
export PATH="$XDG_STATE_HOME/picker-bin:$PATH"
exec "$ROOT/tools/nested/mobius-nested.sh"
