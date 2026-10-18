#!/bin/bash
# Mobius with OpenRouter set up (a made-up key, never sent anywhere: the test only looks at the menu and the saved route).
mkdir -p "$XDG_STATE_HOME/helix/secrets"
cat > "$XDG_STATE_HOME/helix/inference-routing.toml" <<'TOML'
[[backends]]
id = "openrouter"
kind = "openai"
base_url = "https://openrouter.ai/api/v1"
label = "OpenRouter"
api_key_ref = "openrouter"
TOML
printf 'sk-or-test-not-real' > "$XDG_STATE_HOME/helix/secrets/backend-openrouter"
chmod 600 "$XDG_STATE_HOME/helix/secrets/backend-openrouter"
exec "$(dirname "$0")/mobius-nested.sh"
