#!/usr/bin/env bash
# Builds the three packages from the current tree and the binaries already in target/<profile> (no compile, no vendoring), then checks what
# came out: that every installed file is packaged and nothing else is, the dependencies and scriptlets, and that the license lines are current.
# It tests the spec, not a release build: a real build uses the sources from make-sources.sh and the release profile.
#   tools/packaging/rpm-test.sh [profile]        profile defaults to fast; build first with
#                                                  tools/install.sh --build-only --profile fast --require-session && cargo build --profile fast -p niri --bin niri
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
PROFILE=${1:-fast}
VERSION=$(sed -n 's/^Version: *//p' packaging/helix.spec)
TOP=$(mktemp -d)
[ -n "${KEEP_TOP:-}" ] && echo "keeping $TOP" || trap 'rm -rf "$TOP"' EXIT
mkdir -p "$TOP"/{SOURCES,SPECS,BUILD,RPMS,SRPMS,BUILDROOT}

python3 tools/packaging/licenses.py --check packaging/helix.spec

# The tree as it is now (tracked and new files, not what git ignores) stands in for Source0; the two dependency tarballs are not read.
git ls-files -z --cached --others --exclude-standard | tar --null -T - --transform "s,^,helix-desktop-$VERSION/," -czf "$TOP/SOURCES/helix-desktop-$VERSION.tar.gz"
: > "$TOP/SOURCES/helix-desktop-$VERSION-vendor.tar.zst"
: > "$TOP/SOURCES/helix-desktop-$VERSION-node-modules.tar.zst"

# A developer's tree may have a model engine built into third_party/, which puts its path into the inference binaries' RUNPATH. rpmbuild rightly
# rejects that (a package must be built from a clean checkout), and this test is about the spec, so it is let through here only.
export QA_RPATHS=$(( 0x0002 | 0x0008 ))
rpmbuild -bb --nodeps --define "_topdir $TOP" --define "helix_prebuilt 1" --define "helix_profile $PROFILE" \
  --define "helix_target ${CARGO_TARGET_DIR:-$ROOT/target}" packaging/helix.spec >"$TOP/build.log" 2>&1 \
  || { tail -40 "$TOP/build.log"; echo "rpmbuild failed" >&2; exit 1; }

fail=0
check() { if eval "$2"; then echo "ok: $1"; else echo "FAIL: $1"; fail=1; fi; }
HELIX=$(ls "$TOP"/RPMS/*/helix-$VERSION-*.rpm); NIRI=$(ls "$TOP"/RPMS/*/helix-niri-*.rpm); GREETER=$(ls "$TOP"/RPMS/*/helix-greeter-*.rpm)
files() { rpm -qpl "$1" | sort; }
check "three packages were built" '[ -n "$HELIX" ] && [ -n "$NIRI" ] && [ -n "$GREETER" ]'
check "the compositor is /usr/bin/niri and the package replaces niri" 'files "$NIRI" | grep -qx /usr/bin/niri && rpm -qp --conflicts "$NIRI" | grep -qx niri && rpm -qp --provides "$NIRI" | grep -q "^niri ="'
check "the compositor ships its session files" 'for f in /usr/bin/niri-session /usr/share/wayland-sessions/niri.desktop /usr/lib/systemd/user/niri.service /etc/niri/config.kdl; do files "$NIRI" | grep -qx "$f" || exit 1; done'
check "the system niri config names the shell as the trusted client" 'rpm2cpio "$NIRI" | cpio -i --to-stdout ./etc/niri/config.kdl 2>/dev/null | grep -q "trusted-surface" && rpm2cpio "$NIRI" | cpio -i --to-stdout ./etc/niri/config.kdl 2>/dev/null | grep -q "/usr/bin/helix-shell-tauri"'
check "the session units, the preset and the activation files are in helix" 'for f in /usr/lib/systemd/user/helix-shell-bar.service /usr/lib/systemd/user-preset/90-helix.preset /usr/share/dbus-1/services/org.helix.Desktop.service /usr/libexec/helix-system-upgrade /usr/share/polkit-1/actions/org.helix.Shell.upgrade.policy; do files "$HELIX" | grep -qx "$f" || exit 1; done'
check "no compositor binary and no greeter in helix" '! files "$HELIX" | grep -qx /usr/bin/niri && ! files "$HELIX" | grep -qx /usr/bin/helix-greeter'
check "the greeter ships its session script and a greetd config to copy, and needs greetd" 'files "$GREETER" | grep -qx /usr/bin/helix-greeter-session && files "$GREETER" | grep -qx /usr/share/helix-greeter/greetd-config.toml && rpm -qp --requires "$GREETER" | grep -qx greetd'
check "nothing writes /etc/greetd" '! rpm -qpl "$GREETER" "$HELIX" "$NIRI" | grep -q "^/etc/greetd"'
check "the greetd config points at an installed script" 'rpm2cpio "$GREETER" | cpio -i --to-stdout ./usr/share/helix-greeter/greetd-config.toml 2>/dev/null | grep -q "command = \"/usr/bin/helix-greeter-session\""'
check "the post-install script applies the preset (install-user-units) for the four session units" 'rpm -qp --scripts "$HELIX" | grep -q "install-user-units" && grep -c "^enable" packaging/90-helix.preset | grep -qx 4'
check "every unit the preset enables is packaged" 'for u in $(sed -n "s/^enable //p" packaging/90-helix.preset); do files "$HELIX" | grep -qx "/usr/lib/systemd/user/$u" || exit 1; done'
check "helix requires what it runs on" 'rpm -qp --requires "$HELIX" | grep -q pipewire && rpm -qp --requires "$HELIX" | grep -q "webkit2gtk4.1\|libwebkit2gtk"'

echo; echo "helix: $(files "$HELIX" | wc -l) files; helix-niri: $(files "$NIRI" | wc -l); helix-greeter: $(files "$GREETER" | wc -l)"
[ "$fail" = 0 ] && echo "ALL ok" || { echo FAILED; exit 1; }
