#!/bin/bash
# Started by run.sh: helix-idle with short times and stand-in commands that only write to a log.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$XDG_CONFIG_HOME/helix"
export IDLE_HOOK_LOG=$XDG_STATE_HOME/idle-hooks.log
: > "$IDLE_HOOK_LOG"
cat > "$XDG_CONFIG_HOME/helix/idle.toml" <<TOML
lock_after_seconds = 15
screen_off_after_seconds = 21
lock_command = ["$ROOT/tools/nested/idle-hook.sh", "lock"]
screen_off_command = ["$ROOT/tools/nested/idle-hook.sh", "off"]
screen_on_command = ["$ROOT/tools/nested/idle-hook.sh", "on"]
TOML
exec "$ROOT/rust/target/release/helix-idle"
