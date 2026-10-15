#!/usr/bin/env bash
# Runs the checks .github/workflows/ci.yml runs, on this machine, so a PR can be verified without CI minutes.
# Usage: tools/ci-local.sh [job ...]   jobs: i18n format build rust agent config meter  (default: all)
# It uses the existing build-debug tree and installs nothing. The nested smokes pick their own WAYLAND_DISPLAY so they never
# touch a live session's sockets.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
jobs=("$@")
[[ ${#jobs[@]} -eq 0 ]] && jobs=(i18n format build rust agent config meter)
failed=()

run() { # name command...
  local name=$1; shift
  echo "=== $name"
  if "$@"; then echo "--- $name: ok"; else echo "--- $name: FAILED"; failed+=("$name"); fi
}

job_i18n()   { python3 tools/i18n-check.py; }
job_format() {
  command -v clang-format >/dev/null || { echo "clang-format not installed"; return 1; }
  mapfile -d '' files < <(find src \( -name '*.cpp' -o -name '*.h' \) -print0)
  clang-format --dry-run -Werror "${files[@]}"
}
job_build() {
  meson setup --reconfigure build-debug >/dev/null \
    && cargo build --profile fast --quiet $(for p in helix-desktop helix-assistant helix-inference helix-system-upgrade; do \
         [[ -d services/$p ]] && printf -- '-p %s ' "$p"; done) \
    && meson setup --reconfigure build-debug >/dev/null \
    && ninja -C build-debug \
    && sh tests/install_layout_test.sh build-debug \
    && WAYLAND_DISPLAY="wayland-ci-$$" meson test -C build-debug --no-rebuild --print-errorlogs
}
job_rust() {
  cargo test -p helix-shell-core --lib --quiet && cargo test -p helix-theme --quiet && cargo test -p helix-desktop --quiet
}
job_agent()  { WAYLAND_DISPLAY="wayland-ci-$$" ./tools/nested/helix-desktop-agent-integration.sh; }
job_config() {
  WAYLAND_DISPLAY="wayland-ci-$$" \
  HELIX_TEMPLATE_APPLY_BIN="$ROOT/build-debug/helix-template-apply" HELIX_THEME_PALETTE_BIN="$ROOT/build-debug/helix-theme-palette" \
  HELIX_ASSETS_DIR="$ROOT/assets" ./tools/nested/helix-desktop-config-smoke.sh \
    && WAYLAND_DISPLAY="wayland-ci-$$" CONTRACT_TIMEOUT=240 ./tools/nested/helix-desktop-cpp-contract.sh
}
job_meter()  { WAYLAND_DISPLAY="wayland-ci-$$" ./tools/nested/helix-desktop-audio-meter-integration.sh; }

for j in "${jobs[@]}"; do run "$j" "job_$j"; done
echo
if [[ ${#failed[@]} -eq 0 ]]; then echo "ci-local: all passed"; else echo "ci-local: FAILED: ${failed[*]}"; exit 1; fi
