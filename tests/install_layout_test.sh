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

binary=$(find "$stage" -type f -path '*/bin/noctalia' -print -quit)
[ -n "$binary" ] || fail 'installed shell binary is missing'
prefix=${binary%/bin/noctalia}
assets="$prefix/share/noctalia/assets"

for asset in emoji.json fonts/noctalia-tabler.ttf templates/builtin.toml translations/en.json; do
  [ -f "$assets/$asset" ] || fail "installed asset is missing: $asset"
done

[ -f "$prefix/share/applications/dev.noctalia.Noctalia.desktop" ] || fail 'desktop entry is missing'
[ -f "$prefix/share/icons/hicolor/scalable/apps/noctalia.svg" ] || fail 'application icon is missing'
[ -f "$prefix/share/polkit-1/actions/dev.noctalia.Noctalia.upgrade.policy" ] || fail 'upgrade policy is missing'
helper=$(find "$stage" -type f -name noctalia-system-upgrade -print -quit)
[ -n "$helper" ] || fail 'system-upgrade helper is missing'
"$helper" --help > /dev/null 2>&1 || fail 'installed system-upgrade helper could not start'

[ -x "$prefix/bin/helix-service" ] || fail 'desktop service is missing'
[ -f "$prefix/share/dbus-1/services/org.helix.Desktop.service" ] || fail 'D-Bus activation is missing'
[ -f "$prefix/lib/systemd/user/helix-service.service" ] || fail 'systemd user unit is missing'
if [ -x "$prefix/bin/helix-settings" ]; then
  [ -f "$prefix/share/applications/org.helix.Settings.desktop" ] || fail 'Settings desktop entry is missing'
  [ -x "$prefix/bin/helix-system-monitor" ] || fail 'System Monitor is missing'
  [ -f "$prefix/share/applications/org.helix.SystemMonitor.desktop" ] || fail 'System Monitor desktop entry is missing'
  [ -f "$prefix/share/icons/hicolor/scalable/apps/org.helix.SystemMonitor.svg" ] || fail 'System Monitor icon is missing'
  "$prefix/bin/helix-system-monitor" --snapshot > /dev/null || fail 'System Monitor could not read local resources'
fi

version_output=$("$binary" --version) || fail 'installed shell --version failed'
case "$version_output" in
  noctalia\ *) ;;
  *) fail "unexpected version output: $version_output" ;;
esac

mkdir -p "$stage/config" "$stage/state" "$stage/data" "$stage/cache"
XDG_CONFIG_HOME="$stage/config" XDG_STATE_HOME="$stage/state" \
  "$binary" config export full > "$stage/full.toml" || fail 'installed shell could not export its config'
"$binary" config validate "$stage/full.toml" > /dev/null || fail 'installed shell could not validate its exported config'

XDG_CONFIG_HOME="$stage/config" XDG_STATE_HOME="$stage/state" \
  XDG_DATA_HOME="$stage/data" XDG_CACHE_HOME="$stage/cache" \
  "$binary" theme --list-templates > "$stage/templates.txt" 2> "$stage/theme.log" \
  || fail 'installed shell could not load built-in templates'
grep -q 'Niri' "$stage/templates.txt" || fail 'installed shell did not find built-in templates'

printf '%s\n' 'install_layout_test: OK'
