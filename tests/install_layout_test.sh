#!/bin/sh
# Stages tools/install.sh into a temporary directory and checks the installed layout.
#   tests/install_layout_test.sh [profile]     (profile: release or fast; the binaries must already be built)
set -eu

profile=${1:-fast}
root=$(cd "$(dirname "$0")/.." && pwd)
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT HUP INT TERM

fail() {
  printf '%s\n' "install_layout_test: FAIL: $*" >&2
  exit 1
}

"$root/tools/install.sh" --no-build --profile "$profile" --prefix /usr --destdir "$stage" >/dev/null

prefix=$stage/usr

[ -x "$prefix/bin/helix-desktop" ] || fail 'helix-desktop is missing'
[ -x "$prefix/bin/helix-assistant" ] || fail 'helix-assistant is missing'
[ -x "$prefix/bin/helix-inference" ] || fail 'helix-inference is missing'
[ -x "$prefix/libexec/helix-system-upgrade" ] || fail 'helix-system-upgrade is missing'

[ -f "$prefix/share/icons/hicolor/scalable/apps/helix.svg" ] || fail 'application icon is missing'
[ -f "$prefix/share/polkit-1/actions/org.helix.Shell.upgrade.policy" ] || fail 'upgrade policy is missing'
grep -q '/usr/libexec/helix-system-upgrade' "$prefix/share/polkit-1/actions/org.helix.Shell.upgrade.policy" || fail 'upgrade policy does not name the helper'

[ -f "$prefix/share/dbus-1/services/org.helix.Desktop.service" ] || fail 'D-Bus activation is missing'
[ -f "$prefix/lib/systemd/user/helix-service.service" ] || fail 'systemd user unit is missing'
grep -q '/usr/bin/helix-desktop' "$prefix/lib/systemd/user/helix-service.service" || fail 'unit does not start helix-desktop'
grep -q '/usr/bin/helix-desktop' "$prefix/share/dbus-1/services/org.helix.Desktop.service" || fail 'activation does not start helix-desktop'
grep -q '/usr/bin/helix-inference' "$prefix/lib/systemd/user/helix-inference.service" || fail 'unit does not start helix-inference'

[ -f "$prefix/share/helix/msg-risk.toml" ] || fail 'msg-risk protocol file is missing'
[ -f "$prefix/share/helix/tool-hints.toml" ] || fail 'tool-hints protocol file is missing'

# The service finds its assets by walking up from its executable to <prefix>/share/helix.
[ -f "$prefix/share/helix/templates/builtin.toml" ] || fail 'theme templates are missing (the service reads share/helix/templates)'
[ -f "$prefix/share/helix/helix-wallpaper.png" ] || fail 'first-run wallpaper is missing'

# The session half is installed when it was built (a services-only build leaves it out).
if [ -x "$prefix/bin/helix-shell-tauri" ]; then
  [ -x "$prefix/bin/helix-panel" ] || fail 'helix-panel is missing'
  [ -x "$prefix/bin/helix-askpass" ] || fail 'helix-askpass is missing'
  [ -f "$prefix/share/applications/org.helix.Shell.desktop" ] || fail 'shell desktop entry is missing'
  [ -f "$prefix/share/icons/hicolor/scalable/apps/org.helix.Shell.svg" ] || fail 'shell icon is missing'
  for unit in helix-shell-bar helix-shell-panels; do
    grep -q 'ExecStart=/usr/bin/helix-shell-tauri' "$prefix/lib/systemd/user/$unit.service" || fail "$unit does not start the installed shell"
  done
  grep -q 'SUDO_ASKPASS=/usr/bin/helix-askpass' "$prefix/lib/systemd/user/helix-shell-bar.service" || fail 'bar unit does not use the installed askpass'
fi
if [ -x "$prefix/bin/helix-shell-strut" ]; then
  grep -q 'ExecStart=/usr/bin/helix-shell-strut' "$prefix/lib/systemd/user/helix-shell-strut.service" || fail 'strut unit does not start the installed strut'
fi
for app in Settings:helix-settings Software:helix-software SystemMonitor:helix-system-monitor Mobius:helix-mobius; do
  entry=${app%%:*}; binary=${app##*:}
  if [ -x "$prefix/bin/$binary" ]; then
    [ -f "$prefix/share/applications/org.helix.$entry.desktop" ] || fail "$entry desktop entry is missing"
  else
    [ ! -e "$prefix/share/applications/org.helix.$entry.desktop" ] || fail "$entry has a desktop entry but no binary"
  fi
done
if [ -x "$prefix/bin/helix-greeter" ]; then
  [ -x "$prefix/bin/helix-greeter-session" ] || fail 'greeter session script is missing'
  [ -f "$prefix/share/helix-greeter/niri.kdl" ] || fail 'greeter compositor config is missing'
  # The script finds the greeter and its config from where it is installed, whatever the prefix.
  grep -q 'HERE=$(cd "$(dirname "$0")" && pwd)' "$prefix/bin/helix-greeter-session" || fail 'session script is tied to a fixed prefix'
fi
if [ -x "$prefix/bin/helix-settings" ] && [ -x "$prefix/bin/helix-greeter-apply-appearance" ]; then
  : # Settings looks for the appearance helper next to itself; both are in bin/.
elif [ -x "$prefix/bin/helix-settings" ]; then
  fail 'Settings is installed without the greeter appearance helper beside it'
fi

# A staged install must not leave one of our template placeholders behind in a unit, activation file or policy.
if grep -rn '@\(HELIX_BINDIR\|HELPER_PATH\|SHELL_BIN\|STRUT_BIN\|IDLE_BIN\|WALLPAPER_BIN\|ASKPASS\|POLKIT_AGENT\)@' \
     "$prefix/lib/systemd" "$prefix/share/dbus-1" "$prefix/share/polkit-1" >/dev/null 2>&1; then
  fail 'a template placeholder was not substituted'
fi

# --uninstall removes what the install put there.
"$root/tools/install.sh" --uninstall --prefix /usr --destdir "$stage" >/dev/null
left=$(find "$stage" -type f | wc -l)
[ "$left" = 0 ] || fail "uninstall left $left files behind"

printf '%s\n' 'install_layout_test: OK'
