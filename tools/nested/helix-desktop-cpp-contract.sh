#!/usr/bin/env bash
# Runs the C++ desktop service integration test against the Rust helix-desktop binary.
# The test is the behavioural contract the C++ service defines (tests/desktop_service_test.cpp);
# every assertion it trips is a parity gap in services/helix-desktop.
#
#   meson compile -C build-debug desktop_service_test
#   tools/nested/helix-desktop-cpp-contract.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TEST="${DESKTOP_SERVICE_TEST:-$ROOT/build-debug/desktop_service_test}"
BIN="${HELIX_SERVICE_BIN:-$ROOT/target/fast/helix-desktop}"
[[ -x "$TEST" ]] || { echo "build $TEST first (meson compile -C build-debug desktop_service_test)" >&2; exit 1; }
cargo build --profile fast -p helix-desktop --quiet
exec timeout "${CONTRACT_TIMEOUT:-120}" dbus-run-session -- "$TEST" "$BIN"
