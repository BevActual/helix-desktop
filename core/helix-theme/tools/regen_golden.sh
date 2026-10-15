#!/usr/bin/env bash
# Legacy C++ golden regen removed with src/theme/. Use `cargo test -p helix-theme` goldens instead.
set -euo pipefail
echo "regen_golden.sh: C++ theme reference removed; parity is checked by 'cargo test -p helix-theme'." >&2
exit 1
