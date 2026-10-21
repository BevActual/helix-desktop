#!/bin/bash
# tools/nested/settings-tour.sh [dark|light]: photographs every Settings page into /tmp/helix-nested/settings-tour-<theme>/tour-*.png
ROOT=$(cd "$(dirname "$0")/../.." && pwd); THEME=${1:-dark}
rm -rf "/tmp/helix-nested/settings-tour-$THEME"
HELIX_NESTED_SCRIPT="$ROOT/tools/nested/settings-tour-test.sh" timeout 500 "$ROOT/tools/nested/run.sh" "settings-tour-$THEME" "$ROOT/tools/nested/settings-datetime-nested.sh" "HELIX_NESTED_KDL=layout { gaps 0; }" "HELIX_SETTINGS_TEST_THEME=$THEME" >"/tmp/claude-1000/settings-tour-$THEME.log" 2>&1
ls "/tmp/helix-nested/settings-tour-$THEME" | grep -c "tour-" | sed 's/^/pages: /'
