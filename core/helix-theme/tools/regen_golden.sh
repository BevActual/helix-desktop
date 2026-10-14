#!/usr/bin/env bash
# Regenerate golden files from the C++ reference (src/theme). Run from the repo root.
set -euo pipefail
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
g="g++ -std=c++23 -O1 -I src"
$g core/helix-theme/tools/gen_golden.cpp src/theme/color.cpp src/theme/contrast.cpp -o "$out/color"
"$out/color" > core/helix-theme/tests/golden/color_contrast.tsv
$g core/helix-theme/tools/gen_golden_palette.cpp src/theme/fixed_palette.cpp src/theme/color.cpp \
  src/theme/contrast.cpp src/theme/color_roles.cpp -o "$out/palette"
"$out/palette" > core/helix-theme/tests/golden/palette_expand.tsv
$g core/helix-theme/tools/gen_builtin_palettes.cpp src/theme/builtin_palettes.cpp src/theme/fixed_palette.cpp \
  src/theme/color.cpp src/theme/contrast.cpp src/theme/color_roles.cpp -o "$out/builtin"
"$out/builtin" > core/helix-theme/data/builtin_palettes.json
# HCT / tonal palettes: links the in-tree material_color_utilities sources.
mcu=third_party/material_color_utilities
g++ -std=c++23 -O1 -I "$mcu" core/helix-theme/tools/gen_golden_hct.cpp $(find "$mcu/cpp/cam" "$mcu/cpp/palettes" "$mcu/cpp/utils" -name '*.cc') -o "$out/hct"
"$out/hct" > core/helix-theme/tests/golden/hct.tsv
