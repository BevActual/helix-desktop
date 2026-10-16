#!/usr/bin/env bash
# Shared Arch packages for the CI jobs that build the Rust services (desktop, assistant, inference, upgrade helper).
# Extra packages may be passed as arguments (the smoke jobs need dbus + python-gobject).
# The C++ build dependencies (meson, ninja, sdbus-cpp, toml++, nlohmann-json, stb, libqalculate, libical, cups, jemalloc) are gone with the C++ tree.
set -euo pipefail
packages=(
  base-devel git pkgconf clang just
  wayland wayland-protocols
  libglvnd freetype2 fontconfig noto-fonts
  cairo pango harfbuzz
  libxkbcommon glib2
  libsecret libsodium
  libpipewire wireplumber polkit
  pam curl libwebp libjxl libsndfile librsvg
)
packages+=("$@")
for attempt in 1 2 3; do
  pacman -Syu --noconfirm --needed "${packages[@]}" && exit 0
  echo "pacman failed (attempt ${attempt}), retrying..."
  sleep 15
done
exit 1
