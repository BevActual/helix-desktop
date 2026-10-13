#!/bin/bash
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$XDG_STATE_HOME/save-bin"
cat > "$XDG_STATE_HOME/save-bin/zenity" <<'ZEN'
#!/bin/bash
if [[ "$*" == *"--save"* ]]; then
  c="${XDG_STATE_HOME:-/tmp}/save-count"
  n=0; [ -f "$c" ] && n=$(cat "$c")
  n=$((n + 1)); echo "$n" > "$c"
  if [ "$n" -eq 1 ]; then printf '%s/mobius-saved.py\n' "$HOME"; else printf '%s/mobius-snippet.py\n' "$HOME"; fi
  exit 0
fi
exec /usr/bin/zenity "$@"
ZEN
chmod +x "$XDG_STATE_HOME/save-bin/zenity"
export PATH="$XDG_STATE_HOME/save-bin:$PATH"
export HELIX_FAKE_REPLY='Here is a helper:

```python
print("saved")
```
'
exec "$ROOT/tools/nested/mobius-nested.sh"
