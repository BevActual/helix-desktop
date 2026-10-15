# Developer entry point. Cargo builds everything (npm is driven by the apps' build.rs); these recipes only name the common commands.
# Profiles: `fast` (quick, for iterating and tests) and `release` (what gets installed).
set positional-arguments

default:
    @just --list

# Everything in the workspace (needs the Tauri system libraries: WebKitGTK, GTK3, gtk-layer-shell, libsoup3).
build profile="fast":
    cargo build --profile {{profile}} --workspace

# Only what the desktop service and its tests need: no web view libraries required.
build-services profile="fast":
    cargo build --profile {{profile}} -p helix-desktop -p helix-assistant -p helix-inference -p helix-system-upgrade

# Inference fabric, speech worker, and image worker (engines optional).
build-inference profile="fast":
    cargo build --profile {{profile}} -p helix-inference --bin helix-inference --bin helix-transcribe
    cargo build --profile {{profile}} -p helix-image

# Rust template-engine driver for `core/helix-theme/tools/regen_golden_template.sh`.
build-theme-golden:
    cargo build -p helix-theme --bin gen-golden-template

run:
    cargo run -p helix-desktop

# Unit and crate tests, then the integration tests that need a private D-Bus session and the built services.
test profile="fast": (build-services profile)
    cargo test --workspace --quiet
    tools/test-integration.sh

# Only the integration tests (see tools/test-integration.sh for names).
test-integration *names:
    tools/test-integration.sh {{names}}

# Regressions for the GitHub workflow scripts. Pure Python, builds nothing.
test-workflows:
    python3 -m unittest discover -s .github/workflows/scripts -p 'test_*.py'

# Stage an installation in a temporary directory and check the layout.
test-install profile="fast": (build-services profile)
    sh tests/install_layout_test.sh {{profile}}

# The whole local baseline: lints, Rust tests, integration tests and the nested UI suite.
check:
    tools/check-all.sh

# System-level install of the services, units, policy and assets. The session shell and apps: tools/switch-shell.sh install.
install prefix="/usr/local" profile="release":
    tools/install.sh --prefix {{prefix}} --profile {{profile}}

uninstall prefix="/usr/local":
    tools/install.sh --uninstall --prefix {{prefix}}

# Formats the C++ engine shims under services/*/native (the only C++ left). Rust is formatted with `cargo fmt`.
format:
    #!/usr/bin/env bash
    set -euo pipefail
    command -v clang-format >/dev/null || { echo "clang-format not installed" >&2; exit 1; }
    find services -path '*/native/*' \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 -r clang-format -i

# Tauri session shell: core lib tests, nested agent-ipc/launcher, agent policy (safe with Cursor open).
verify-shell-integrations:
    tools/verify-shell-integrations.sh
