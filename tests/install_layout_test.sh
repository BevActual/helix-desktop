#!/bin/sh

set -eu

build_dir=$1
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT HUP INT TERM

fail() {
  printf '%s\n' "install_layout_test: FAIL: $*" >&2
  exit 1
}

meson install --no-rebuild --destdir "$stage" -C "$build_dir" --quiet

prefix=$(find "$stage" -type f -path '*/bin/helix-service' -print -quit)
[ -n "$prefix" ] || fail 'installed helix-service is missing'
prefix=${prefix%/bin/helix-service}

[ -f "$prefix/share/icons/hicolor/scalable/apps/helix.svg" ] || fail 'application icon is missing'
[ -f "$prefix/share/polkit-1/actions/org.helix.Shell.upgrade.policy" ] || fail 'upgrade policy is missing'
helper=$(find "$stage" -type f -name helix-system-upgrade -print -quit)
[ -n "$helper" ] || fail 'system-upgrade helper is missing'
"$helper" --help > /dev/null 2>&1 || fail 'installed system-upgrade helper could not start'

[ -x "$prefix/bin/helix-service" ] || fail 'desktop service is missing'
[ -x "$prefix/bin/helix-template-apply" ] || fail 'template apply helper is missing'
[ -f "$prefix/share/dbus-1/services/org.helix.Desktop.service" ] || fail 'D-Bus activation is missing'
[ -f "$prefix/lib/systemd/user/helix-service.service" ] || fail 'systemd user unit is missing'
[ -f "$prefix/share/helix/msg-risk.toml" ] || fail 'msg-risk protocol file is missing'
[ -f "$prefix/share/helix/tool-hints.toml" ] || fail 'tool-hints protocol file is missing'

if [ -x "$prefix/bin/helix-settings" ]; then
  [ -f "$prefix/share/applications/org.helix.Settings.desktop" ] || fail 'Settings desktop entry is missing'
  [ -x "$prefix/bin/helix-system-monitor" ] || fail 'System Monitor is missing'
  [ -f "$prefix/share/applications/org.helix.SystemMonitor.desktop" ] || fail 'System Monitor desktop entry is missing'
  [ -f "$prefix/share/icons/hicolor/scalable/apps/org.helix.SystemMonitor.svg" ] || fail 'System Monitor icon is missing'
  "$prefix/bin/helix-system-monitor" --snapshot > /dev/null || fail 'System Monitor could not read local resources'
  for app in Software Phone; do
    case "$app" in Software) executable=helix-software ;; Phone) executable=helix-phone ;; esac
    [ -x "$prefix/bin/$executable" ] || fail "$app application is missing"
    [ -f "$prefix/share/applications/org.helix.$app.desktop" ] || fail "$app desktop entry is missing"
    "$prefix/bin/$executable" --help > /dev/null || fail "$app command-line entry could not start"
  done
fi

printf '%s\n' 'install_layout_test: OK'
