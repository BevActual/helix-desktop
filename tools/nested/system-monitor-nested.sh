#!/bin/bash
# Helix System Monitor against a fake /proc tree.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${HELIX_MONITOR_BIN:-$(ls -t "$ROOT/target/fast/helix-system-monitor" "$ROOT/target/release/helix-system-monitor" 2>/dev/null | head -1)}
FAKE=$XDG_STATE_HOME/fake-proc
mkdir -p "$FAKE/net" "$FAKE/1" "$FAKE/42"
# Minimal /proc shapes the pure parsers accept.
printf 'cpu  100 0 0 100 0 0 0 0\n' > "$FAKE/stat"
printf 'MemTotal:       2048000 kB\nMemAvailable:   1024000 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n' > "$FAKE/meminfo"
printf 'Inter-|   Receive                                                |  Transmit\n face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n  eth0: 1000 0 0 0 0 0 0 0 500 0 0 0 0 0 0 0\n    lo: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n' > "$FAKE/net/dev"
printf '0.50 0.40 0.30 1/100 1\n' > "$FAKE/loadavg"
# pid (name) state … utime stime … threads … rss
printf '1 (init) S 0 0 0 0 -1 0 0 0 0 0 1 1 0 0 20 0 1 0 0 0 10\n' > "$FAKE/1/stat"
printf '42 (helix-shell) R 1 1 1 0 -1 0 0 0 0 0 20 10 0 0 20 0 4 0 0 0 200\n' > "$FAKE/42/stat"
mkdir -p "$XDG_DATA_HOME/helix/screen-time"
printf 'org.helix.Settings 300\norg.helix.Mobius 120\n' > "$XDG_DATA_HOME/helix/screen-time/report"
export HELIX_MONITOR_PROC="$FAKE"
export HELIX_MONITOR_SCREEN_TIME="$XDG_DATA_HOME/helix/screen-time/report"
export HELIX_UI_LAYOUT_LOG="$XDG_STATE_HOME/layout.json"
echo $$ > "$XDG_STATE_HOME/shell.pid"
exec "$BIN"
