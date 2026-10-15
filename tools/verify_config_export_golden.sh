#!/usr/bin/env bash
# Fail if Rust default export or frozen parity fixtures drift.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GOLDEN="$ROOT/core/helix-config-export/tests/golden/config_export_minimal.toml"

cargo build -p helix-config-export --bin helix-rust-config-export --quiet

cargo test -p helix-config-export config_export_defaults::tests::ported_schema_sections_match_golden_export --quiet
cargo test -p helix-config-export --test config_export_golden rust_export_matches_minimal_golden --quiet
cargo test -p helix-config-export --test config_export_golden --quiet

BIN="${HELIX_RUST_CONFIG_EXPORT_BIN:-${HELIX_CONFIG_EXPORT_BIN:-}}"
if [[ -z "$BIN" ]]; then
  for candidate in \
    "$ROOT/target/debug/helix-rust-config-export" \
    "$ROOT/target/fast/helix-rust-config-export"; do
    if [[ -x "$candidate" ]]; then
      BIN="$candidate"
      break
    fi
  done
fi

if [[ -n "$BIN" && -x "$BIN" ]]; then
  TMP="$(mktemp)"
  trap 'rm -f "$TMP"' EXIT
  HELIX_RUST_CONFIG_EXPORT_BIN="$BIN" "$ROOT/tools/gen_config_export_golden.sh" "$TMP"
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
