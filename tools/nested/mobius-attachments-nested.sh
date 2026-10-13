#!/bin/bash
# Stand-in picker selects fixture paths; the app's real reader, preview, composer and sender run normally.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$XDG_STATE_HOME/picker-bin" "$XDG_STATE_HOME/attachment-fixtures"
python3 - "$XDG_STATE_HOME/attachment-fixtures" <<'PY'
import pathlib,sys
p=pathlib.Path(sys.argv[1])
(p/'long.txt').write_text('é'*7000)
(p/'second.txt').write_text('b'*3000)
(p/'third.txt').write_text('c'*1000)
(p/'binary.txt').write_bytes(b'x'*9000+b'\0')
(p/'id_ed25519').write_text('-----BEGIN OPENSSH PRIVATE KEY-----\nSECRET_FIXTURE\n')
PY
cat > "$XDG_STATE_HOME/picker-bin/zenity" <<'PICKER'
#!/bin/bash
echo picker-called >> "$XDG_STATE_HOME/picker.log"
printf '%s\n' "$XDG_STATE_HOME/attachment-fixtures/long.txt" "$XDG_STATE_HOME/attachment-fixtures/second.txt" "$XDG_STATE_HOME/attachment-fixtures/third.txt" "$XDG_STATE_HOME/attachment-fixtures/binary.txt" "$XDG_STATE_HOME/attachment-fixtures/id_ed25519"
PICKER
chmod +x "$XDG_STATE_HOME/picker-bin/zenity"
export PATH="$XDG_STATE_HOME/picker-bin:$PATH"
exec "$ROOT/tools/nested/mobius-nested.sh"
