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

# A staged install must not leave a template placeholder behind.
if grep -rn '@HELIX_BINDIR@\|@HELPER_PATH@' "$prefix" >/dev/null 2>&1; then
  fail 'a template placeholder was not substituted'
fi

printf '%s\n' 'install_layout_test: OK'
