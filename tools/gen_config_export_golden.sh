#!/usr/bin/env bash
# Capture helix-config-export default effective export for Rust parity (schema defaults + minimal fixture).
# Regenerate when C++ config_export::serialize defaults change.
# Usage: tools/gen_config_export_golden.sh [output.toml]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/core/helix-shell-core/tests/golden/config_export_minimal.toml}"
BIN="${HELIX_CONFIG_EXPORT_BIN:-}"

if [[ -z "$BIN" ]]; then
  for candidate in \
    "$ROOT/build-debug/helix-config-export" \
    "$ROOT/build-fast/helix-config-export" \
    "$ROOT/_build/helix-config-export"; do
    if [[ -x "$candidate" ]]; then
      BIN="$candidate"
      break
    fi
  done
fi

if [[ -z "$BIN" || ! -x "$BIN" ]]; then
  echo "error: build helix-config-export first (just build debug) or set HELIX_CONFIG_EXPORT_BIN" >&2
  exit 1
fi

TMP_CONFIG="$(mktemp -d)"
TMP_STATE="$(mktemp -d)"
mkdir -p "$TMP_CONFIG/helix" "$TMP_STATE/helix"
cat >"$TMP_CONFIG/helix/config.toml" <<'EOF'
[theme]
mode = "dark"

[bar.default]
enabled = true
thickness = 34
EOF
printf '\n' >"$TMP_STATE/helix/settings.toml"

export XDG_CONFIG_HOME="$TMP_CONFIG"
export XDG_STATE_HOME="$TMP_STATE"
export HELIX_CONFIG_HOME="$TMP_CONFIG"
export HELIX_STATE_HOME="$TMP_STATE"

mkdir -p "$(dirname "$OUT")"
"$BIN" >"$OUT"
rm -rf "$TMP_CONFIG" "$TMP_STATE"
echo "wrote $OUT"
