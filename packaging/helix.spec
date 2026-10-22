# Helix: a Wayland desktop on a niri fork, with a shell, login screen and apps that people and AI agents share.
#
# Three packages from one source tree:
#   helix          the services, the session shell and the apps (MIT)
#   helix-niri     the compositor, a niri fork (GPL-3.0-or-later); replaces the distribution's niri
#   helix-greeter  the login screen for greetd
#
# Sources are made by tools/packaging/make-sources.sh (git archive, cargo vendor, and node_modules), so a build needs no network. See
# PACKAGING.md for the decisions behind this file and how to build it. The License: lines are generated:
#   tools/packaging/licenses.py --write packaging/helix.spec

%global debug_package %{nil}
%global __strip /bin/true

# Overridable for a local check (tools/packaging/rpm-test.sh builds the profile `fast` and skips %build).
%{!?helix_profile: %global helix_profile release}
%{!?helix_prebuilt: %global helix_prebuilt 0}

%bcond_with check

Name:           helix
Version:        0.1.0
Release:        1%{?dist}
Summary:        Wayland desktop with a shell, login screen and apps that people and AI agents share

SourceLicense:  MIT AND GPL-3.0-or-later
License:        ((MIT OR Apache-2.0) AND Unicode-3.0) AND (0BSD OR MIT OR Apache-2.0) AND (Apache-2.0 AND ISC) AND (Apache-2.0 AND MIT) AND (Apache-2.0 OR BSL-1.0) AND (Apache-2.0 OR ISC OR MIT) AND (Apache-2.0 OR MIT) AND (Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT) AND (Apache-2.0 WITH LLVM-exception) AND (BSD-2-Clause OR Apache-2.0 OR MIT) AND (BSD-3-Clause AND MIT) AND (BSD-3-Clause OR Apache-2.0) AND (BSD-3-Clause OR MIT OR Apache-2.0) AND (BSD-3-Clause OR MIT) AND (CC0-1.0 OR MIT-0 OR Apache-2.0) AND (MIT OR Apache-2.0 OR LGPL-2.1-or-later) AND (MIT OR Apache-2.0 OR Zlib) AND (MIT OR Apache-2.0) AND (MIT OR Zlib OR Apache-2.0) AND (Unlicense OR MIT) AND (Zlib OR Apache-2.0 OR MIT) AND Apache-2.0 AND BSD-3-Clause AND CDLA-Permissive-2.0 AND ISC AND MIT AND MPL-2.0 AND Unicode-3.0 AND Zlib
URL:            https://github.com/BevActual/helix-desktop
Source0:        helix-desktop-%{version}.tar.gz
Source1:        helix-desktop-%{version}-vendor.tar.zst
Source2:        helix-desktop-%{version}-node-modules.tar.zst

# The web views bundle native node modules, and only x86_64 has been built and tested.
ExclusiveArch:  x86_64

BuildRequires:  rust >= 1.85
BuildRequires:  cargo
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  clang
BuildRequires:  pkgconf-pkg-config
BuildRequires:  nodejs
BuildRequires:  npm
BuildRequires:  zstd
BuildRequires:  systemd-rpm-macros
BuildRequires:  desktop-file-utils
# The shell, greeter and apps (Tauri)
BuildRequires:  pkgconfig(webkit2gtk-4.1)
BuildRequires:  pkgconfig(gtk+-3.0)
BuildRequires:  pkgconfig(libsoup-3.0)
BuildRequires:  pkgconfig(gtk-layer-shell-0)
BuildRequires:  pkgconfig(librsvg-2.0)
BuildRequires:  pkgconfig(libsecret-1)
# The services
BuildRequires:  pkgconfig(libpipewire-0.3)
BuildRequires:  pkgconfig(dbus-1)
# The compositor
BuildRequires:  pkgconfig(udev)
BuildRequires:  pkgconfig(gbm)
BuildRequires:  pkgconfig(xkbcommon)
BuildRequires:  pkgconfig(libinput)
BuildRequires:  pkgconfig(libseat)
BuildRequires:  pkgconfig(libdisplay-info)
BuildRequires:  pkgconfig(wayland-server)
BuildRequires:  pkgconfig(systemd)
BuildRequires:  pkgconfig(pangocairo)
BuildRequires:  pkgconfig(cairo-gobject)
BuildRequires:  pam-devel

# The compositor is the one Helix needs, and the shell's approval dialog is only protected by it.
Recommends:     helix-niri = %{version}-%{release}
Requires:       pipewire
Requires:       wireplumber
Requires:       polkit
Requires:       dbus-common
Requires:       python3
# Pages that are empty or say "not available" without these, never an error.
Recommends:     NetworkManager
Recommends:     bluez
Recommends:     flatpak
Recommends:     PackageKit
Recommends:     fwupd
Recommends:     xdg-desktop-portal-gtk
Recommends:     gnome-keyring

%description
Helix is a Wayland desktop built on the niri compositor. This package holds its services (desktop service, assistant, inference service),
the session shell (bar, dock, panels), and the apps (Settings, Mobius, Software, System Monitor).

The model engines (llama.cpp, whisper.cpp, stable-diffusion.cpp) are not part of this package; the inference service runs without them
using only its scripted backend.

%package niri
Summary:        Helix compositor (a niri fork)
License:        ((MIT OR Apache-2.0) AND BSD-3-Clause) AND ((MIT OR Apache-2.0) AND Unicode-3.0) AND (0BSD OR MIT OR Apache-2.0) AND (Apache-2.0 AND MIT) AND (Apache-2.0 OR MIT) AND (Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT) AND (Apache-2.0 WITH LLVM-exception) AND (BSD-2-Clause OR Apache-2.0 OR MIT) AND (BSD-3-Clause OR MIT OR Apache-2.0) AND (MIT OR Apache-2.0 OR LGPL-2.1-or-later) AND (MIT OR Apache-2.0 OR Zlib) AND (MIT OR Apache-2.0) AND (MIT OR Zlib OR Apache-2.0) AND (Unlicense OR MIT) AND (Zlib OR Apache-2.0 OR MIT) AND Apache-2.0 AND BSD-2-Clause AND BSD-3-Clause AND GPL-3.0-or-later AND ISC AND MIT AND MPL-2.0 AND Unicode-3.0 AND Zlib
# The binary is /usr/bin/niri, so it can replace the distribution's compositor without changing what launches it.
Conflicts:      niri
Provides:       niri = %{version}-%{release}
Requires:       mesa-dri-drivers
Requires:       mesa-libEGL
# Loaded through dlopen
Requires:       libwayland-server
Requires:       xwayland-satellite >= 0.7
Recommends:     xdg-desktop-portal-gtk
Recommends:     xdg-desktop-portal-gnome
Recommends:     alacritty
Recommends:     fuzzel

%description niri
The compositor Helix runs on: niri with liquid-glass effects, an agent desk, and a trusted-surface role that keeps the shell's approval dialog
on top, out of screen captures and closed to synthetic input. It installs /usr/bin/niri and a system configuration (/etc/niri/config.kdl)
that already names the Helix shell as the trusted client. A person with their own ~/.config/niri/config.kdl needs the same block; see
%{_datadir}/helix/niri/helix-trusted-surface.kdl.

%package greeter
Summary:        Helix login screen for greetd
License:        ((MIT OR Apache-2.0) AND Unicode-3.0) AND (0BSD OR MIT OR Apache-2.0) AND (Apache-2.0 AND MIT) AND (Apache-2.0 OR MIT) AND (Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT) AND (Apache-2.0 WITH LLVM-exception) AND (BSD-3-Clause AND MIT) AND (BSD-3-Clause OR MIT OR Apache-2.0) AND (BSD-3-Clause OR MIT) AND (CC0-1.0 OR MIT-0 OR Apache-2.0) AND (MIT OR Apache-2.0 OR LGPL-2.1-or-later) AND (MIT OR Apache-2.0 OR Zlib) AND (MIT OR Apache-2.0) AND (MIT OR Zlib OR Apache-2.0) AND (Unlicense OR MIT) AND (Zlib OR Apache-2.0 OR MIT) AND Apache-2.0 AND BSD-3-Clause AND ISC AND MIT AND MPL-2.0 AND Unicode-3.0 AND Zlib
Requires:       greetd
Requires:       helix-niri = %{version}-%{release}

%description greeter
The Helix login screen: a private compositor with the greeter as its only client, falling back to greetd's text login if it cannot start.
Installing this package does not change greetd. To use it, copy %{_datadir}/helix-greeter/greetd-config.toml over /etc/greetd/config.toml (keep
the old one), and enable greetd.service.

%prep
%setup -q -n helix-desktop-%{version}
%if !%{helix_prebuilt}
# Dependencies, so the build needs no network.
tar --zstd -xf %{SOURCE1}
tar --zstd -xf %{SOURCE2}
mkdir -p .cargo
cat cargo-vendor-config.toml >> .cargo/config.toml
%endif

%build
%if !%{helix_prebuilt}
export CARGO_FLAGS="--locked --offline"
export CARGO_HOME=%{_builddir}/cargo-home
export NIRI_BUILD_COMMIT=%{version}
# The workspace's release profile (opt-level, LTO) is the source of truth for code generation; the distribution's RUSTFLAGS would override it
# and add full debug info to every binary. Keep the link hardening.
export RUSTFLAGS="-Clink-arg=-Wl,-z,relro -Clink-arg=-Wl,-z,now"
# The web views are built by each app's build.rs when node_modules exists (it does: Source2).
tools/install.sh --build-only --require-session --profile %{helix_profile} --prefix %{_prefix}
cargo build --profile %{helix_profile} --locked --offline -p niri --bin niri
%endif

%install
DESTDIR=%{buildroot} CARGO_TARGET_DIR="%{?helix_target}%{!?helix_target:target}" \
  tools/install.sh --no-build --require-session --profile %{helix_profile} --prefix %{_prefix}

# The speech and image workers need an engine built by hand, which this package does not carry, so they are not shipped either.
rm -f %{buildroot}%{_bindir}/helix-transcribe %{buildroot}%{_bindir}/helix-image

# The user preset for the session units.
install -Dm644 packaging/90-helix.preset %{buildroot}%{_prefix}/lib/systemd/user-preset/90-helix.preset
install -Dm644 packaging/90-helix-geoclue.conf %{buildroot}%{_sysconfdir}/geoclue/conf.d/90-helix.conf
install -Dm644 packaging/helix-lock.pam %{buildroot}%{_sysconfdir}/pam.d/helix-lock

# The compositor, laid out the way the distribution's own niri package does.
NIRI="%{?helix_target}%{!?helix_target:target}/%{helix_profile}/niri"
install -Dm755 "$NIRI" %{buildroot}%{_bindir}/niri
install -Dm755 -t %{buildroot}%{_bindir} compositor/resources/niri-session
install -Dm644 -t %{buildroot}%{_datadir}/wayland-sessions compositor/resources/niri.desktop
install -Dm644 -t %{buildroot}%{_datadir}/xdg-desktop-portal compositor/resources/niri-portals.conf
install -Dm644 -t %{buildroot}%{_userunitdir} compositor/resources/niri.service compositor/resources/niri-shutdown.target
install -d %{buildroot}%{bash_completions_dir} %{buildroot}%{fish_completions_dir} %{buildroot}%{zsh_completions_dir}
%{buildroot}%{_bindir}/niri completions bash > %{buildroot}%{bash_completions_dir}/niri
%{buildroot}%{_bindir}/niri completions fish > %{buildroot}%{fish_completions_dir}/niri.fish
%{buildroot}%{_bindir}/niri completions zsh > %{buildroot}%{zsh_completions_dir}/_niri
# The shell's approval dialog is a trusted surface only when the compositor is told which executable is the shell.
install -d %{buildroot}%{_datadir}/helix/niri %{buildroot}%{_sysconfdir}/niri
cat > %{buildroot}%{_datadir}/helix/niri/helix-trusted-surface.kdl <<'KDL'
// The shell's approval dialog is a trusted surface: kept on top, capture-proof, closed to synthetic input (docs/design/trusted-surface.md).
trusted-surface {
    namespace "helix-tauri-confirmer"
    exe "%{_bindir}/helix-shell-tauri"
}
KDL
{ cat compositor/resources/default-config.kdl; printf '\n'; cat %{buildroot}%{_datadir}/helix/niri/helix-trusted-surface.kdl; } \
  > %{buildroot}%{_sysconfdir}/niri/config.kdl

# greetd: a configuration to copy, never written over the administrator's own.
install -d %{buildroot}%{_datadir}/helix-greeter
cat > %{buildroot}%{_datadir}/helix-greeter/greetd-config.toml <<'TOML'
[terminal]
vt = 1

[default_session]
command = "%{_bindir}/helix-greeter-session"
user = "greetd"
TOML

install -D -m 0644 packaging/50-helix-greeter-power.rules %{buildroot}%{_datadir}/polkit-1/rules.d/50-helix-greeter-power.rules

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/*.desktop
%if %{with check}
cargo test --profile %{helix_profile} --locked --offline -p helix-base -p helix-agent -p helix-config -p helix-config-export -p helix-models \
  -p helix-compositor -p helix-shell-model -p helix-chat -p helix-software-model -p helix-monitor-model -p helix-wallpaper-model -p helix-nightlight-model
%endif

%post
%systemd_user_post helix-shell-strut.service helix-shell-bar.service helix-wallpaper.service helix-idle.service helix-nightlight.service

%preun
%systemd_user_preun helix-shell-strut.service helix-shell-bar.service helix-wallpaper.service helix-idle.service helix-nightlight.service

%files
%license LICENSE
%doc README.md PACKAGING.md BUILDING.md
%{_bindir}/helix-desktop
%{_bindir}/helix-assistant
%{_bindir}/helix-inference
%{_bindir}/helix-panel
%{_bindir}/helix-askpass
%{_bindir}/helix-shell-tauri
%{_bindir}/helix-shell-strut
%{_bindir}/helix-wallpaper
%{_bindir}/helix-idle
%{_bindir}/helix-lock
%config(noreplace) %{_sysconfdir}/pam.d/helix-lock
%{_bindir}/helix-nightlight
%{_bindir}/helix-settings
%{_bindir}/helix-mobius
%{_bindir}/helix-software
%{_bindir}/helix-system-monitor
%{_bindir}/helix-greeter-apply-appearance
%{_libexecdir}/helix-system-upgrade
%{_userunitdir}/helix-service.service
%{_userunitdir}/helix-inference.service
%{_userunitdir}/helix-assistant-warm.service
%{_userunitdir}/helix-shell-strut.service
%{_userunitdir}/helix-shell-bar.service
%{_userunitdir}/helix-shell-panels.service
%{_userunitdir}/helix-idle.service
%{_userunitdir}/helix-nightlight.service
%{_userunitdir}/helix-wallpaper.service
%{_prefix}/lib/systemd/user-preset/90-helix.preset
%config(noreplace) %{_sysconfdir}/geoclue/conf.d/90-helix.conf
%{_datadir}/dbus-1/services/org.helix.Desktop.service
%{_datadir}/dbus-1/services/org.helix.Inference.service
%{_datadir}/polkit-1/actions/org.helix.Shell.upgrade.policy
%{_datadir}/applications/org.helix.*.desktop
%{_datadir}/icons/hicolor/scalable/apps/helix.svg
%{_datadir}/icons/hicolor/scalable/apps/org.helix.Shell.svg
%{_datadir}/icons/hicolor/scalable/apps/org.helix.SystemMonitor.svg
%dir %{_datadir}/helix
%{_datadir}/helix/msg-risk.toml
%{_datadir}/helix/tool-hints.toml
%{_datadir}/helix/helix-wallpaper.png
%{_datadir}/helix/templates
%dir %{_datadir}/helix/niri

%files niri
%license compositor/LICENSE
%doc compositor/README.md
%doc compositor/resources/default-config.kdl
%{_bindir}/niri
%{_bindir}/niri-session
%{_datadir}/wayland-sessions/niri.desktop
%dir %{_datadir}/xdg-desktop-portal
%{_datadir}/xdg-desktop-portal/niri-portals.conf
%{_userunitdir}/niri.service
%{_userunitdir}/niri-shutdown.target
%{bash_completions_dir}/niri
%{fish_completions_dir}/niri.fish
%{zsh_completions_dir}/_niri
%{_datadir}/helix/niri/helix-trusted-surface.kdl
%dir %{_sysconfdir}/niri
%config(noreplace) %{_sysconfdir}/niri/config.kdl

%files greeter
%license LICENSE
%{_bindir}/helix-greeter
%{_bindir}/helix-greeter-session
%{_datadir}/helix-greeter
%{_datadir}/polkit-1/rules.d/50-helix-greeter-power.rules

%changelog
* Fri Oct 16 2026 Andrew Bevington <andrewlancebevington@gmail.com> - 0.1.0-1
- First package: services, session shell and apps; the compositor; the greeter.
