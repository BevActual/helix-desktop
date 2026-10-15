#!/usr/bin/env bash
# Regenerates tests/golden/template/* from the Rust template engine.
set -euo pipefail
cd "$(dirname "$0")/../../.."
cargo build -q -p helix-theme --bin gen-golden-template
driver=target/debug/gen-golden-template
dest=core/helix-theme/tests/golden/template
rm -rf "$dest/full"
"$driver" assets/templates "$dest/synthetic.tpl" "$dest/full" >"$dest/hashes.tsv"
find "$dest/full" -name '*_t:*' -delete
