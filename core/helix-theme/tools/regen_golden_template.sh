#!/usr/bin/env bash
# Regenerates tests/golden/template/*.tsv from the C++ TemplateEngine.
set -euo pipefail
cd "$(dirname "$0")/../../.."
B=${BUILD_DIR:-build-debug}
meson compile -C "$B" helix-gen-golden-template
driver="$B/helix-gen-golden-template"
dest=core/helix-theme/tests/golden/template
rm -rf "$dest/full"
"$driver" assets/templates "$dest/synthetic.tpl" "$dest/full" >"$dest/hashes.tsv"
find "$dest/full" -name '*_t:*' -delete
