#!/usr/bin/env bash
# The integration tests that need built binaries and a private D-Bus session: the desktop service, assistant and inference service
# against each other, the Agent Surface, the audio meter, and the layering and contract checks. This is what `meson test` ran.
#
#   tools/test-integration.sh [name ...]       (no names: everything whose requirements are present)
#
# Binaries come from target/fast, else target/debug (cargo build --profile fast -p helix-desktop -p helix-assistant -p helix-inference).
# A test whose binaries or tools are missing is skipped and said so, never silently passed.
# helix-system-upgrade is also looked up next to the other binaries, for the install layout check.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

bin() { # name -> path of the newest built binary, or empty
  for profile in fast debug; do
    [ -x "target/$profile/$1" ] && { echo "$ROOT/target/$profile/$1"; return; }
  done
}
DESKTOP=$(bin helix-desktop)
ASSISTANT=$(bin helix-assistant)
INFERENCE=$(bin helix-inference)
RISK=$ROOT/protocols/ipc/msg-risk.toml
have_gio() { python3 -c 'from gi.repository import Gio, GLib' 2>/dev/null; }
have_bus() { command -v dbus-run-session >/dev/null; }
have_pw()  { for t in pipewire pw-cli pw-link pw-dump; do command -v "$t" >/dev/null || return 1; done; }
AGENT_PY=$ROOT/tools/helix-agent/.venv/bin/python
have_agent_py() { [ -x "$AGENT_PY" ]; }

only=("$@")
want() { [ ${#only[@]} -eq 0 ] && return 0; local n; for n in "${only[@]}"; do [ "$n" = "$1" ] && return 0; done; return 1; }

passed=(); failed=(); skipped=()
run() { # name timeout 'requirement expression' -- command...
  local name=$1 limit=$2 requirement=$3
  shift 3; [ "${1:-}" = "--" ] && shift
  want "$name" || return 0
  if ! eval "$requirement" >/dev/null 2>&1; then echo "--- $name: skipped (requirements missing)"; skipped+=("$name"); return 0; fi
  echo "=== $name"
  if timeout "$limit" "$@"; then echo "--- $name: ok"; passed+=("$name"); else echo "--- $name: FAILED"; failed+=("$name"); fi
}
need() { [ -n "$1" ]; }   # a built binary was found

run layering 60 true -- python3 tools/check-layers.py "$ROOT"
run desktop_dbus_contract 60 true -- python3 tests/desktop_dbus_contract_test.py "$ROOT"
run template_undo_signal 60 true -- bash tests/template_undo_signal_test.sh "$ROOT/assets/templates"
run install_layout 60 'need "$DESKTOP" && need "$ASSISTANT" && need "$INFERENCE" && [ -x "$(dirname "$DESKTOP")/helix-system-upgrade" ]' -- \
  sh tests/install_layout_test.sh "$(basename "$(dirname "$DESKTOP")")"

run inference_service_integration 300 'have_bus && have_gio && need "$INFERENCE"' -- \
  dbus-run-session -- python3 tests/inference_service_integration.py "$INFERENCE"
run assistant_integration 120 'have_bus && have_gio && need "$ASSISTANT" && need "$DESKTOP" && need "$INFERENCE"' -- \
  dbus-run-session -- python3 tests/assistant_integration.py "$ASSISTANT" "$DESKTOP" "$INFERENCE" "$ROOT"
run desktop_agent_manager_integration 90 'have_bus && have_gio && need "$DESKTOP"' -- \
  dbus-run-session -- python3 tests/desktop_agent_manager_integration.py "$DESKTOP" "$RISK"
run desktop_agent_integration 90 'have_bus && have_gio && need "$DESKTOP"' -- \
  dbus-run-session -- python3 tests/desktop_agent_integration.py "$DESKTOP" "$RISK"
run desktop_audio_meter_integration 60 'have_bus && have_gio && have_pw && need "$DESKTOP"' -- \
  dbus-run-session -- python3 tests/desktop_audio_meter_integration.py "$DESKTOP" "$ROOT/tests/desktop_audio/pipewire.conf"
run helix_desktop_config_smoke 120 'have_bus && have_gio && need "$DESKTOP"' -- \
  env HELIX_SERVICE_BIN="$DESKTOP" HELIX_ASSETS_DIR="$ROOT/assets" dbus-run-session -- tools/nested/helix-desktop-config-smoke.sh
run helix_desktop_location_smoke 120 'have_bus && have_gio && need "$DESKTOP"' -- \
  env HELIX_SERVICE_BIN="$DESKTOP" HELIX_ASSETS_DIR="$ROOT/assets" dbus-run-session -- tools/nested/helix-desktop-location-smoke.sh
run helix_desktop_configuration_contract 180 'have_bus && have_gio && need "$DESKTOP"' -- \
  dbus-run-session -- tools/nested/helix-desktop-configuration-contract.sh
run helix_agent_adapter_integration 120 'have_bus && have_agent_py && need "$DESKTOP"' -- \
  dbus-run-session -- "$AGENT_PY" tests/helix_agent_adapter_integration.py "$ROOT" "$DESKTOP"
run helix_agent_policy 60 have_agent_py -- "$AGENT_PY" tests/helix_agent_policy_test.py "$ROOT"

echo
echo "passed: ${#passed[@]}  skipped: ${#skipped[@]}  failed: ${#failed[@]}"
[ ${#skipped[@]} -gt 0 ] && echo "skipped: ${skipped[*]}"
[ ${#failed[@]} -gt 0 ] && { echo "FAILED: ${failed[*]}"; exit 1; }
exit 0
