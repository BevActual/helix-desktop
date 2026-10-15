#!/usr/bin/env bash
# Fail if Rust default export or frozen parity fixtures drift (C++ check optional).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GOLDEN="$ROOT/core/helix-shell-core/tests/golden/config_export_minimal.toml"

cargo test -p helix-shell-core desktop::config_export_defaults::tests::ported_schema_sections_match_golden_export --quiet
cargo test -p helix-shell-core --test config_export_golden rust_export_matches_golden_without_cpp_helper --quiet
cargo test -p helix-shell-core --test config_export_golden --quiet

BIN="${HELIX_CONFIG_EXPORT_BIN:-}"
if [[ -z "$BIN" ]]; then
  for candidate in \
    "$ROOT/target/debug/helix-rust-config-export" \
    "$ROOT/target/fast/helix-rust-config-export" \
    "$ROOT/build-debug/helix-rust-config-export" \
    "$ROOT/build-debug/helix-config-export" \
    "$ROOT/build-fast/helix-config-export" \
    "$ROOT/_build/helix-config-export"; do
    if [[ -x "$candidate" ]]; then
      BIN="$candidate"
      break
    fi
  done
fi

if [[ -n "$BIN" && -x "$BIN" ]]; then
  TMP="$(mktemp)"
  trap 'rm -f "$TMP"' EXIT
  HELIX_CONFIG_EXPORT_BIN="$BIN" "$ROOT/tools/gen_config_export_golden.sh" "$TMP"
  if ! python3 -c "
import tomllib, sys
from pathlib import Path
golden = tomllib.loads(Path('$GOLDEN').read_text())
got = tomllib.loads(Path('$TMP').read_text())
sys.exit(0 if golden == got else 1)
"; then
    echo "error: config_export_minimal.toml drift vs export CLI; rerun tools/gen_config_export_golden.sh" >&2
    exit 1
  fi
  echo "config_export_minimal.toml matches export CLI"
else
  echo "skip export CLI minimal golden diff (helix-rust-config-export not built)"
fi

echo "config export golden checks passed"
