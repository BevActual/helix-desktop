#!/usr/bin/env python3
"""Install the model engine's shared libraries into a private directory next to helix-inference.

The engine (llama.cpp and ggml) is built by hand into third_party/llama-install and is not a system package, so it is installed
under <prefix>/lib64/helix where only helix-inference looks for it (via its install rpath). Only the libraries the service links
against are copied; the engine's command-line tools and development files are not. Symlinks stay symlinks.

Usage (meson install script): install_engine.py <engine lib dir> <destination, relative to the install prefix>
"""
import os
import pathlib
import re
import shutil
import sys

source, relative = pathlib.Path(sys.argv[1]), sys.argv[2]
destdir = os.environ.get("DESTDIR", "")
prefix = os.environ["MESON_INSTALL_PREFIX"]
target = pathlib.Path(destdir + prefix) / relative if not destdir else pathlib.Path(destdir) / prefix.lstrip("/") / relative
target.mkdir(parents=True, exist_ok=True)

WANTED = re.compile(r"^lib(llama|mtmd|ggml|ggml-base|ggml-cpu|ggml-vulkan)\.so(\.\d+(\.\d+)*)?$")
copied = 0
for entry in sorted(source.iterdir()):
    if not WANTED.match(entry.name) or entry.name.endswith(".so"):  # the unversioned name is for linking, not running
        continue
    destination = target / entry.name
    if destination.is_symlink() or destination.exists():
        destination.unlink()
    if entry.is_symlink():
        os.symlink(os.readlink(entry), destination)
    else:
        shutil.copy2(entry, destination)
    copied += 1
if copied == 0:
    sys.exit(f"no engine libraries found in {source}")
print(f"installed {copied} engine files to {target}")
