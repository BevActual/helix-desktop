#!/usr/bin/env bash
# Runs the checks .github/workflows/ci.yml runs, on this machine, so a PR can be verified without CI minutes.
# Usage: tools/ci-local.sh [job ...]   jobs: i18n build rust agent config meter  (default: all)
# It builds into target/ and installs nothing. The nested smokes pick their own WAYLAND_DISPLAY so they never
# touch a live session's sockets.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
jobs=("$@")
[[ ${#jobs[@]} -eq 0 ]] && jobs=(i18n build rust agent config meter)
failed=()

run() { # name command...
  local name=$1; shift
  echo "=== $name"
  if "$@"; then echo "--- $name: ok"; else echo "--- $name: FAILED"; failed+=("$name"); fi
}

job_i18n()   { python3 tools/i18n-check.py; }
job_build() {
  cargo build --profile fast --quiet -p helix-desktop -p helix-assistant -p helix-inference -p helix-system-upgrade \
    && sh tests/install_layout_test.sh fast \
    && WAYLAND_DISPLAY="wayland-ci-$$" tools/test-integration.sh
}
job_rust() {
  cargo test -p helix-base -p helix-agent -p helix-config -p helix-config-export -p helix-models -p helix-compositor -p helix-shell-model -p helix-chat -p helix-software-model -p helix-monitor-model -p helix-wallpaper-model --quiet && cargo test -p helix-theme --quiet && cargo test -p helix-desktop --quiet
}
job_agent()  { WAYLAND_DISPLAY="wayland-ci-$$" ./tools/nested/helix-desktop-agent-integration.sh; }
job_config() {
  WAYLAND_DISPLAY="wayland-ci-$$" \
  HELIX_ASSETS_DIR="$ROOT/assets" \
  ./tools/nested/helix-desktop-config-smoke.sh \
    && ./tools/nested/helix-desktop-configuration-contract.sh
}
job_meter()  { WAYLAND_DISPLAY="wayland-ci-$$" ./tools/nested/helix-desktop-audio-meter-integration.sh; }

for j in "${jobs[@]}"; do run "$j" "job_$j"; done
echo
if [[ ${#failed[@]} -eq 0 ]]; then echo "ci-local: all passed"; else echo "ci-local: FAILED: ${failed[*]}"; exit 1; fi
