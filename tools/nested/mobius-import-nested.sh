#!/bin/bash
# Mobius with a model file waiting in the home folder and a file picker that chooses it (and a second picker run that cancels).
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$XDG_STATE_HOME/pick-bin" "$HOME/Downloads"
python3 - "$HOME/Downloads" <<'PY'
import sys
head = b"GGUF" + bytes(4096)
open(sys.argv[1] + "/My Tiny Model (Q4).gguf", "wb").write(head)
open(sys.argv[1] + "/not-a-model.gguf", "wb").write(b"x" * 4096)
PY
# The first run picks the real file, the second the fake one, the third cancels.
cat > "$XDG_STATE_HOME/pick-bin/zenity" <<'PICK'
#!/bin/bash
COUNT="$XDG_STATE_HOME/pick-count"; n=$(cat "$COUNT" 2>/dev/null || echo 0); echo $((n + 1)) > "$COUNT"
case $n in
  0) printf '%s/Downloads/My Tiny Model (Q4).gguf\n' "$HOME" ;;
  1) printf '%s/Downloads/not-a-model.gguf\n' "$HOME" ;;
  *) exit 1 ;;
esac
PICK
chmod +x "$XDG_STATE_HOME/pick-bin/zenity"
export PATH="$XDG_STATE_HOME/pick-bin:$PATH"
exec "$ROOT/tools/nested/mobius-nested.sh"
