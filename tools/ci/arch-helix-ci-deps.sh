#!/usr/bin/env bash
# Shared Arch packages for helix-shell C++ CI (build + Rust desktop smokes with helpers).
# Extra packages may be passed as arguments (the smoke job needs dbus + python-gobject;
# the Build job must not install them or extra meson integration tests get enabled).
set -euo pipefail
packages=(
  base-devel git meson ninja pkgconf
  wayland wayland-protocols
  libglvnd freetype2 fontconfig noto-fonts
  cairo pango harfbuzz
  libxkbcommon glib2
  libsecret libsodium
  sdbus-cpp libpipewire wireplumber polkit
  pam curl libwebp libjxl libsndfile librsvg
  libqalculate libxml2
  tomlplusplus libical libcups
  nlohmann-json stb
  jemalloc just clang
)
packages+=("$@")
for attempt in 1 2 3; do
  pacman -Syu --noconfirm --needed "${packages[@]}" && exit 0
  echo "pacman failed (attempt ${attempt}), retrying..."
  sleep 15
done
exit 1
