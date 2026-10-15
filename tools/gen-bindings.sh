#!/bin/bash
# TypeScript types for the Rust views the React apps receive, written to core/shared-ui/react/bindings/.
# Run after changing a view struct that has `#[cfg_attr(feature = "ts", derive(ts_rs::TS), ts(export))]`.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/core/shared-ui/react/bindings"
rm -rf "$OUT"; mkdir -p "$OUT"
# ts-rs writes a type's file from a test in the crate that defines it, so each crate with exports runs its own.
for crate in helix-agent helix-compositor helix-config helix-models helix-shell-model; do
  TS_RS_EXPORT_DIR="$OUT" cargo test --manifest-path "$ROOT/Cargo.toml" -p "$crate" --features ts --lib export_bindings -- --quiet >/dev/null
done
ls "$OUT"
