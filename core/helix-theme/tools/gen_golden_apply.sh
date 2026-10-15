#!/usr/bin/env bash
# Deprecated: apply goldens are owned by Rust (`cargo test -p helix-theme matches_cpp_template_apply`).
# To refresh expected trees, edit scenarios under tests/golden/apply/ and run that test.
set -euo pipefail
echo "gen_golden_apply.sh: use 'cargo test -p helix-theme matches_cpp_template_apply' instead." >&2
exit 1
