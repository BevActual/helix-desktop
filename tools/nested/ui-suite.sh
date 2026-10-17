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
pick shell-auto-theme && run shell-auto-theme shell-auto-theme-test.sh tauri-fake-desktop.sh
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
pick settings-sound && run settings-sound settings-sound-test.sh settings-fake-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick mobius-act     && run mobius-act mobius-act-test.sh mobius-act-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick settings-dock  && run settings-dock settings-dock-test.sh settings-fake-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick settings-notifications && run settings-notifications settings-notifications-test.sh settings-fake-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick notifications-prefs && run notifications-prefs notifications-prefs-test.sh tauri-shell.sh
pick ui-scale          && run ui-scale ui-scale-test.sh system-monitor-scale-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick ui-theme          && run ui-theme ui-theme-test.sh system-monitor-scale-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick dock-prefs     && run dock-prefs dock-prefs-test.sh dock-nested.sh
pick mobius         && run mobius mobius-test.sh mobius-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick greeter        && run greeter greeter-test.sh greeter-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick greeter-session && run greeter-session greeter-session-test.sh greeter-session-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick software       && run software software-test.sh software-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick software-real && run software-real software-real-test.sh software-real-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick software-rpm   && run software-rpm software-rpm-test.sh software-rpm-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick monitor        && run monitor system-monitor-test.sh system-monitor-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick models         && run models models-test.sh mobius-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick mobius-real    && [ -n "${HELIX_SUITE_HEAVY:-}" -o ${#sel[@]} -gt 0 ] && run mobius-real mobius-real-test.sh mobius-real-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick mobius-dialogs && run mobius-dialogs mobius-dialogs-test.sh mobius-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
pick mobius-lab     && run mobius-lab mobius-lab-test.sh mobius-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }" HELIX_MOBIUS_LOG_PROMPT=1
pick mobius-pictures && run mobius-pictures mobius-pictures-test.sh mobius-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }" HELIX_NESTED_SEED_PICTURE=1
pick mobius-dictate && run mobius-dictate mobius-dictate-test.sh mobius-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }" HELIX_NESTED_SEED_AUDIO=1
pick mobius-library && run mobius-library mobius-library-test.sh mobius-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }"
if pick mobius-image-generation; then
  image_models=${HELIX_MODEL_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/helix/models}
  if [ -f "$image_models/image/stable-diffusion-v2-1-turbo-Q4_0.gguf" ]; then
    run mobius-image-generation mobius-image-generation-test.sh mobius-nested.sh "HELIX_MODEL_DIR=$image_models" "HELIX_NESTED_KDL=layout { gaps 0; }" HELIX_FAKE_IMAGE_DELAY_MS=400 RUST_LOG=niri=info WEBKIT_DISABLE_DMABUF_RENDERER=1 WEBKIT_DISABLE_COMPOSITING_MODE=1 LIBGL_ALWAYS_SOFTWARE=1
  else
    echo "skip  mobius-image-generation (SD-Turbo is not installed)"
  fi
fi
if pick mobius-analysis; then
  analysis_config=${HELIX_TEST_ANALYSIS_CONFIG:-${XDG_DATA_HOME:-$HOME/.local/share}/helix/analysis-tools/config.json}
  if [ -f "$analysis_config" ]; then
    run mobius-analysis mobius-analysis-test.sh mobius-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }" "HELIX_TEST_ANALYSIS_CONFIG=$analysis_config" RUST_LOG=niri=info WEBKIT_DISABLE_DMABUF_RENDERER=1 WEBKIT_DISABLE_COMPOSITING_MODE=1 LIBGL_ALWAYS_SOFTWARE=1 HELIX_FAKE_DELAY_MS=10
  else
    echo "skip  mobius-analysis (optional analysis tools are not installed)"
  fi
fi

if pick mobius-tables; then
  run mobius-tables mobius-tables-test.sh mobius-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }" "HELIX_FAKE_REPLY=| Model | Cost |
| :--- | ---: |
| **Small** | \`2|3\` |
| Large | 5 |" HELIX_FAKE_DELAY_MS=10 RUST_LOG=niri=info WEBKIT_DISABLE_DMABUF_RENDERER=1 WEBKIT_DISABLE_COMPOSITING_MODE=1 LIBGL_ALWAYS_SOFTWARE=1
fi
pick mobius-search && run mobius-search mobius-search-test.sh mobius-search-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }" RUST_LOG=niri=info WEBKIT_DISABLE_DMABUF_RENDERER=1 WEBKIT_DISABLE_COMPOSITING_MODE=1 LIBGL_ALWAYS_SOFTWARE=1
pick mobius-attachments && run mobius-attachments mobius-attachments-test.sh mobius-attachments-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }" RUST_LOG=niri=info WEBKIT_DISABLE_DMABUF_RENDERER=1 WEBKIT_DISABLE_COMPOSITING_MODE=1 LIBGL_ALWAYS_SOFTWARE=1 HELIX_FAKE_DELAY_MS=10
pick mobius-context && run mobius-context mobius-context-test.sh mobius-context-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }" RUST_LOG=niri=info WEBKIT_DISABLE_DMABUF_RENDERER=1 WEBKIT_DISABLE_COMPOSITING_MODE=1 LIBGL_ALWAYS_SOFTWARE=1 HELIX_MOBIUS_LOG_PROMPT=1 HELIX_FAKE_DELAY_MS=400
pick mobius-welcome && run mobius-welcome mobius-welcome-test.sh mobius-welcome-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }" RUST_LOG=niri=info WEBKIT_DISABLE_DMABUF_RENDERER=1 WEBKIT_DISABLE_COMPOSITING_MODE=1 LIBGL_ALWAYS_SOFTWARE=1 HELIX_NESTED_SEED_AUDIO=1 HELIX_FAKE_DELAY_MS=10
pick mobius-web && run mobius-web mobius-web-test.sh mobius-web-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }" RUST_LOG=niri=info WEBKIT_DISABLE_DMABUF_RENDERER=1 WEBKIT_DISABLE_COMPOSITING_MODE=1 LIBGL_ALWAYS_SOFTWARE=1 HELIX_FAKE_DELAY_MS=10 "HELIX_FAKE_REPLY=A sourced explanation [1]."
pick mobius-tools && run mobius-tools mobius-tools-test.sh mobius-tools-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }" RUST_LOG=niri=info WEBKIT_DISABLE_DMABUF_RENDERER=1 WEBKIT_DISABLE_COMPOSITING_MODE=1 LIBGL_ALWAYS_SOFTWARE=1 HELIX_FAKE_DELAY_MS=10 "HELIX_FAKE_REPLY=A sourced explanation [1]."
pick mobius-reply-save && run mobius-reply-save mobius-reply-save-test.sh mobius-reply-save-nested.sh "HELIX_NESTED_KDL=layout { gaps 0; }" RUST_LOG=niri=info WEBKIT_DISABLE_DMABUF_RENDERER=1 WEBKIT_DISABLE_COMPOSITING_MODE=1 LIBGL_ALWAYS_SOFTWARE=1 HELIX_FAKE_DELAY_MS=10
