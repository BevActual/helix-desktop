#!/usr/bin/env python3
"""Embed a text file as a C++ constant: embed_text.py <input> <output.h> <identifier>."""
import pathlib
import sys

src, dst, name = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3]
text = src.read_text(encoding="utf-8")
assert ")XML\"" not in text, "input contains the raw-string terminator"
dst.write_text(
    "// Generated from %s by tools/scripts/embed_text.py. Do not edit.\n"
    "#pragma once\n\nnamespace helix::protocols {\n"
    "inline constexpr const char* %s = R\"XML(%s)XML\";\n}\n" % (src.name, name, text.rstrip("\n"))
)
