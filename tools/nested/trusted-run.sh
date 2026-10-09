#!/bin/bash
# Runs trusted-attack-test.sh against a compositor build: `stock` (the installed niri, no trusted-surface block) or `protected` (the fork in
# ../helix-niri/target/debug/niri with the shell's dialog configured as a trusted surface).
#   tools/nested/trusted-run.sh protected|stock|wrong-exe [script]   (script: trusted-attack-test.sh, or protection-test.sh)
# `wrong-exe` is the fork with the right namespace but another executable listed: no protection must be given (a copy of the shell, or a program
# that only knows the namespace, is not trusted).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MODE=${1:-protected}
SCRIPT=${2:-trusted-attack-test.sh}
SHELL_BIN=$(ls -t "$ROOT/rust/target/fast/helix-shell-tauri" "$ROOT/rust/target/release/helix-shell-tauri" 2>/dev/null | head -1)
export HELIX_SHELL_BIN=$SHELL_BIN TRUSTED_EXPECT=$MODE
# Approvals are not allowed on an unprotected compositor here (the default for a person).
export HELIX_ALLOW_UNPROTECTED_APPROVALS=${HELIX_ALLOW_UNPROTECTED_APPROVALS:-0}
[ "$MODE" = wrong-exe ] && export TRUSTED_EXPECT=stock
KDL=""
if [ "$MODE" = protected ] || [ "$MODE" = wrong-exe ]; then
  export HELIX_NIRI_BIN="${HELIX_NIRI_BIN:-$ROOT/../helix-niri/target/debug/niri}"
  EXE=$SHELL_BIN; [ "$MODE" = wrong-exe ] && EXE=/usr/bin/not-the-shell
  KDL="trusted-surface { namespace \"helix-tauri-confirmer\"; exe \"$EXE\"; }"
fi
rm -rf "/tmp/helix-nested/trusted-$MODE"
# The probe is a copy of the interpreter holding observe, act and confirm, so a confirm-tier action reaches the approval dialog.
PROBE_DIR=$(mktemp -d); cp "$(readlink -f "$(command -v python3)")" "$PROBE_DIR/python3"
export HELIX_PROBE_PYTHON="$PROBE_DIR/python3" HELIX_NESTED_ASSISTANT="$PROBE_DIR/python3" HELIX_NESTED_ASSISTANT_SCRIPT="$ROOT/tools/nested/agent-probe.py" HELIX_NESTED_ASSISTANT_TIERS="observe act confirm"
HELIX_NESTED_SCRIPT="$ROOT/tools/nested/$SCRIPT" "$ROOT/tools/nested/run.sh" "trusted-$MODE" "$ROOT/tools/nested/tauri-shell.sh" ${KDL:+"HELIX_NESTED_KDL=$KDL"}
