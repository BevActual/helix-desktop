#!/bin/bash
# Runs every nested UI test in turn against one shell binary (default: the fast build) and prints one line per test.
#   tools/nested/ui-suite.sh [name ...]      names: see the pick lines below (launcher qs net bt ctl cc ... mobius)
#   (wallhaven-test.sh needs the internet and is run by hand.)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
# Whichever shell build was made last (fast or release), unless HELIX_SHELL_BIN says otherwise.
export HELIX_SHELL_BIN="${HELIX_SHELL_BIN:-$(ls -t "$ROOT/target/fast/helix-shell-tauri" "$ROOT/target/release/helix-shell-tauri" 2>/dev/null | head -1)}"
# A shell started from a launched program must not decide the tests' roles for them.
unset HELIX_SHELL_ROLE HELIX_SHELL_CONFIRMER
run() {  # name script program [env...]
  local name=$1 script=$2 program=$3; shift 3
  local out=/tmp/claude-1000/suite-$name.log
  # Tests count the actions a run sent, so each starts from an empty scratch directory (this exact path, made by this script).
  rm -rf "/tmp/helix-nested/suite-$name"
  HELIX_NESTED_SCRIPT="$ROOT/tools/nested/$script" timeout 420 "$ROOT/tools/nested/run.sh" "suite-$name" "$ROOT/tools/nested/$program" "$@" >"$out" 2>&1
  if grep -qE 'FAIL|TIMEOUT|NOT FOUND|FAILED' "$out"; then echo "FAIL  $name   ($out)"; grep -E 'FAIL|TIMEOUT|NOT FOUND' "$out" | head -3; else echo "ok    $name"; fi
}
sel=("$@")
pick() { [ ${#sel[@]} -eq 0 ] && return 0; for n in "${sel[@]}"; do [ "$n" = "$1" ] && return 0; done; return 1; }
pick launcher       && run launcher launcher-test.sh tauri-shell.sh
pick qs             && run qs quick-settings-test.sh tauri-fake-desktop.sh
pick net            && run net network-test.sh tauri-fake-desktop.sh
pick bt             && run bt bluetooth-test.sh tauri-fake-desktop.sh
pick ctl            && run ctl shell-control-test.sh tauri-shell.sh
pick cc             && run cc control-center-test.sh tauri-shell.sh HELIX_NESTED_PRIVATE_AUDIO=1
pick notifications  && run notifications notifications-test.sh tauri-shell.sh
pick trusted-attack && run trusted-attack trusted-attack-test.sh tauri-shell.sh
pick agent-ipc      && run agent-ipc agent-ipc-test.sh tauri-fake-desktop.sh
pick today          && run today today-test.sh tauri-shell.sh
pick activity       && run activity activity-test.sh tauri-shell.sh
pick qi             && run qi quick-insert-test.sh tauri-fake-desktop.sh
pick wp             && run wp wallpaper-test.sh wallpaper-nested.sh
pick keys           && run keys keys-test.sh tauri-shell.sh HELIX_NESTED_PRIVATE_AUDIO=1
pick dock           && run dock dock-test.sh dock-nested.sh
pick ws             && run ws workspaces-test.sh tauri-fake-desktop.sh
pick tray           && run tray tray-test.sh tauri-fake-desktop.sh
pick idle           && run idle idle-test.sh idle-nested.sh
pick island         && run island island-test.sh tauri-shell.sh
pick settings       && run settings settings-test.sh settings-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick settings-net   && run settings-net settings-net-test.sh settings-fake-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick settings-dock  && run settings-dock settings-dock-test.sh settings-fake-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick dock-prefs     && run dock-prefs dock-prefs-test.sh dock-nested.sh
pick mobius         && run mobius mobius-test.sh mobius-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick greeter        && run greeter greeter-test.sh greeter-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick greeter-session && run greeter-session greeter-session-test.sh greeter-session-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick software       && run software software-test.sh software-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick monitor        && run monitor system-monitor-test.sh system-monitor-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
