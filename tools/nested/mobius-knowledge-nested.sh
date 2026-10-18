#!/bin/bash
# Mobius with a notes folder waiting in the home folder and a folder picker that chooses it. The meaning-search model is used when it is
# at $HELIX_EMBED_DIR or ~/.cache/helix-experiments/embed/bge-small-en-v1.5.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$XDG_STATE_HOME/pick-bin" "$HOME/notes/sub" "$HOME/notes/node_modules/pkg"
cat > "$HOME/notes/cooking.md" <<'MD'
# Cooking

A good tomato sauce simmers slowly for an hour. Add basil at the end so it stays fresh, and a little sugar if the tomatoes are sharp.

## Pasta

Salt the water well. Cook the pasta one minute short of the packet time and finish it in the sauce.
MD
cat > "$HOME/notes/sub/bikes.md" <<'MD'
# Bicycle maintenance

Lubricate the chain every few hundred kilometres and wipe off the old oil first. Check the tyre pressure before every ride, and replace brake pads when the grooves are gone.
MD
printf 'module.exports = "ignored"\n' > "$HOME/notes/node_modules/pkg/index.js"
cat > "$XDG_STATE_HOME/pick-bin/zenity" <<'PICK'
#!/bin/bash
printf '%s/notes\n' "$HOME"
PICK
chmod +x "$XDG_STATE_HOME/pick-bin/zenity"
export PATH="$XDG_STATE_HOME/pick-bin:$PATH"
REAL_HOME=$(getent passwd "$(id -u)" | cut -d: -f6)
EMBED=${HELIX_EMBED_DIR:-$REAL_HOME/.cache/helix-experiments/embed/bge-small-en-v1.5}
[ -f "$EMBED/model.safetensors" ] && export HELIX_EMBED_DIR=$EMBED
exec "$ROOT/tools/nested/mobius-nested.sh"
