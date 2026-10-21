#!/bin/bash
# Started by run.sh: the scripted desktop service with day and night times that make it night now (sunset three hours ago, sunrise in three), and
# helix-nightlight set to follow the sun. The service asks for the location every second so the test does not wait a minute.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$XDG_CONFIG_HOME/helix"
echo '{"version":1,"enabled":true,"warmth":3000,"scheduled":true,"sun":true,"start":20,"end":7}' > "$XDG_CONFIG_HOME/helix/nightlight.json"
read -r SUNSET SUNRISE <<<"$(python3 -c 'import datetime as d; n=d.datetime.now(); f=lambda t:(t).strftime("%H:%M"); print(f(n-d.timedelta(hours=3)), f(n+d.timedelta(hours=3)))')"
export HELIX_FAKE_CUSTOM_SCHEDULE=1 HELIX_FAKE_SUNSET=$SUNSET HELIX_FAKE_SUNRISE=$SUNRISE HELIX_NIGHTLIGHT_LOCATION_SECONDS=1
echo "$SUNSET $SUNRISE" > "$XDG_STATE_HOME/night-times"
python3 "$ROOT/tools/nested/fake-desktop.py" "$XDG_STATE_HOME" > "$XDG_STATE_HOME/fake-desktop.log" 2>&1 &
for _ in $(seq 1 50); do grep -q "ready" "$XDG_STATE_HOME/fake-desktop.log" 2>/dev/null && break; sleep 0.1; done
BIN=$(ls -t "$ROOT/target/fast/helix-nightlight" "$ROOT/target/release/helix-nightlight" 2>/dev/null | head -1)
exec "$BIN"
