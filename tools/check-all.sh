#!/bin/bash
# The whole baseline in one command: lints, Rust tests, the C++ tests, and the nested UI suite. Stops at the first red step.
#   tools/check-all.sh [--no-nested] [--no-meson]
# Leftovers of earlier nested runs are stopped first (only processes whose command line is a /tmp/helix-nested one).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
nested=1; meson_tests=1
for arg in "$@"; do case "$arg" in --no-nested) nested=0;; --no-meson) meson_tests=0;; esac; done
step() { printf '\n== %s\n' "$*"; }
step "stop leftovers of earlier nested runs"
for p in $(pgrep -x niri 2>/dev/null); do
  case "$(tr '\0' ' ' < /proc/$p/cmdline)" in *"-c /tmp/helix-nested/"*) kill -9 "$p" 2>/dev/null || true;; esac
done
step "cargo clippy (warnings are errors)"
cargo clippy --release --workspace --all-targets -- -D warnings
step "cargo test"
cargo test --release --workspace
if [ $meson_tests = 1 ]; then
  BUILD=$(ls -d "$ROOT/build-debug" "$ROOT/build-native-clean" 2>/dev/null | head -1)
  [ -n "$BUILD" ] || { echo "no meson build directory (build-debug or build-native-clean)"; exit 1; }
  step "meson test ($BUILD)"
  nice -n 10 meson test -C "$BUILD" --print-errorlogs
fi
if [ $nested = 1 ]; then
  step "build what the nested tests run"
  cargo build --release --workspace
  step "nested UI suite"
  out=$(HELIX_SHELL_BIN="$ROOT/target/release/helix-shell-tauri" tools/nested/ui-suite.sh | tee /dev/stderr)
  if printf '%s\n' "$out" | grep -q '^FAIL'; then echo "nested suite has failures"; exit 1; fi
fi
step "all green"
