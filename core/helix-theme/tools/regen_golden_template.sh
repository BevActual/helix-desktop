#!/usr/bin/env bash
# Regenerates tests/golden/template/*.tsv from the C++ TemplateEngine. Needs a configured
# build-debug (meson compile -C build-debug helix-template-apply) so libhelix_core.a exists.
set -euo pipefail
cd "$(dirname "$0")/../../.."
B=${BUILD_DIR:-build-debug}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
# Reuse the exact flags and libraries of helix-template-apply.
cc=$(ninja -C "$B" -t commands helix-template-apply | grep "template_apply_main.cpp" | head -1)
flags=${cc#c++ }
flags=${flags%% -MD*}
flags=${flags//-Ihelix-template-apply.p/}
(cd "$B" && eval "c++ $flags -c ../core/helix-theme/tools/gen_golden_template.cpp -o '$out/driver.o'")
link=$(ninja -C "$B" -t commands helix-template-apply | tail -1)
libs=${link#*template_apply_main.cpp.o }
(cd "$B" && eval "c++ -o '$out/driver' '$out/driver.o' $libs")
dest=core/helix-theme/tests/golden/template
rm -rf "$dest/full"
"$out/driver" assets/templates "$dest/synthetic.tpl" "$dest/full" >"$dest/hashes.tsv"
# Only the synthetic template keeps full outputs; the bundled ones are compared by hash.
find "$dest/full" -name '*_t:*' -delete
