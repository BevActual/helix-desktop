#!/usr/bin/env bash
# Fail if config_export_minimal.toml drifts from helix-config-export (run after C++ schema changes).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GOLDEN="$ROOT/core/helix-shell-core/tests/golden/config_export_minimal.toml"
TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT
"$ROOT/tools/gen_config_export_golden.sh" "$TMP"
if ! diff -u "$GOLDEN" "$TMP"; then
  echo "error: golden drift; rerun tools/gen_config_export_golden.sh" >&2
  exit 1
fi
echo "config_export_minimal.toml matches helix-config-export"
