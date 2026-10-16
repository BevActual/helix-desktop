#!/bin/bash
# Fast regression bundle for Tauri session shell agent surface, launcher, and policy.
# Safe while Cursor is open (nested compositor only; does not restart the live shell).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

NIRI_DEBUG="${CARGO_TARGET_DIR:-$ROOT/target}/debug/niri"
if [ ! -x "$NIRI_DEBUG" ]; then
  echo "== compositor debug niri (for nested runs)"
  cargo build -p niri --bin niri
fi
export HELIX_NIRI_BIN="${HELIX_NIRI_BIN:-$NIRI_DEBUG}"

echo "== core crates (lib)"
cargo test -p helix-base -p helix-agent -p helix-config -p helix-config-export -p helix-models -p helix-compositor -p helix-shell-model -p helix-chat -p helix-software-model -p helix-monitor-model --quiet

echo "== nested agent-ipc (incl. helix-panel status / desktop-capture)"
HELIX_NESTED_SCRIPT=tools/nested/agent-ipc-test.sh \
  tools/nested/run.sh agent-ipc tools/nested/tauri-fake-desktop.sh

echo "== nested launcher (incl. act agent)"
HELIX_NESTED_SCRIPT=tools/nested/launcher-test.sh \
  tools/nested/run.sh launcher tools/nested/tauri-shell.sh

if [ -x "$ROOT/tools/helix-agent/.venv/bin/python" ]; then
  echo "== agent policy (python)"
  "$ROOT/tools/helix-agent/.venv/bin/python" tests/helix_agent_policy_test.py "$ROOT"
else
  echo "== agent policy (python, skip: no tools/helix-agent/.venv)"
fi

echo "ALL shell integration checks ok"
