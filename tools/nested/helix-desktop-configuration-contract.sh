#!/usr/bin/env bash
# Rust Configuration1 contract (subset of tests/desktop_service_test.cpp).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${HELIX_SERVICE_BIN:-$ROOT/target/fast/helix-desktop}"

if [[ "${_HELIX_CONFIGURATION_CONTRACT_DBUS:-}" != "1" ]]; then
  cargo build --profile fast -p helix-desktop --quiet
  command -v dbus-run-session >/dev/null || {
    echo "dbus-run-session required" >&2
    exit 1
  }
  # Never inherit the host data dirs — dbus would activate an installed Desktop.service.
  exec dbus-run-session -- env -u HELIX_STATE_HOME \
    _HELIX_CONFIGURATION_CONTRACT_DBUS=1 \
    HELIX_SERVICE_BIN="$BIN" \
    XDG_DATA_DIRS=/var/empty \
    "$0"
fi

export HELIX_RUN_CONFIGURATION_CONTRACT=1
export HELIX_DISABLE_CPP_THEME_HELPERS="${HELIX_DISABLE_CPP_THEME_HELPERS:-1}"
export XDG_DATA_DIRS=/var/empty
unset HELIX_STATE_HOME
exec cargo test -p helix-desktop --test configuration_contract -- --nocapture --test-threads=1
