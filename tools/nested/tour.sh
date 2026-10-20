#!/bin/bash
# Photographs Mobius's screens at a window width and theme: tools/nested/tour.sh 1100 dark. Output: /tmp/helix-nested/tour-<width>-<theme>/tour-*.png
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
W=${1:-1100}; THEME=${2:-dark}
KDL=$(printf 'HELIX_NESTED_KDL=layout {\n gaps 0\n default-column-width { fixed %s; }\n}' "$W")
rm -rf "/tmp/helix-nested/tour-$W-$THEME"
HELIX_NESTED_SCRIPT="$ROOT/tools/nested/mobius-tour-test.sh" timeout 400 "$ROOT/tools/nested/run.sh" "tour-$W-$THEME" "$ROOT/tools/nested/mobius-acp-nested.sh" "$KDL" "HELIX_MOBIUS_TEST_THEME=$THEME" "HELIX_FAKE_REPLY=You can use **cron** or a **systemd timer**.

- \`cron\` is simple: run \`crontab -e\`
- a timer is better for logging and missed runs

\`\`\`sh
crontab -e
\`\`\`" "HELIX_FAKE_DELAY_MS=5" >"/tmp/claude-1000/tour-$W-$THEME.log" 2>&1
ls "/tmp/helix-nested/tour-$W-$THEME" | grep -c "tour-" | sed 's/^/screens: /'
