# Helix desktop transition handoff

This document is a working map for the next implementation pass. The product
contract is in `helix-hig.md`, the service contract and validation notes are in
`desktop-service.md`, and `desktop-migration.json` is the machine-checked
inventory. The desktop cutover remains **off**. Do not remove an existing tab,
plugin entry, setting, command alias, or configuration path until its inventory
entry has a working destination and `status: verified`, except for the explicitly
retired legacy Settings editors described below.

On 2026-10-05 the user explicitly requested removal of **all** legacy shell Settings
editors, including plugins, widgets, hooks and Niri before replacement parity.
That retirement is intentional; it does not certify migration completeness. The
existing configuration remains active, compatibility settings contexts launch GTK
Settings, and unavailable editors are identified in the native app. Rebuild those
capabilities in Settings rather than restoring a legacy editor. Source snapshots
of the replaced pane are saved under `docs/design/archive/retired-settings-source`.

## What runs today

- `helix-service` owns configuration writes, appearance exports, audio mixer,
  NetworkManager and BlueZ access. It is session D-Bus activatable without the
  shell. Its interfaces are rooted at `org.helix.Desktop` and versioned `1`.
- `helix-settings` is a single-instance GTK4/libadwaita app. Its pages are
  Appearance, Desktop & Dock, Notifications, Accessibility, Sound, Network,
  Bluetooth, Privacy & Security, System, and About. Desktop-only pages are
  capability-gated; unavailable services leave navigation accessible and edits
  disabled. The Appearance page has visual mode choices, palette and wallpaper
  previews, a searchable palette gallery, local wallpaper library and Wallhaven
  browser. Wallhaven credentials stay in the service.
- `helix-system-monitor` is a separate adaptive app with Overview, Processes,
  and Screen Time. The last page reads local reports; process actions are not
  implemented.
- Control Center Home still owns quick shell controls, including Wi-Fi,
  Bluetooth, sound, brightness, media, capture, phone and session actions and an
  available Tailscale toggle. Native detail routes exist for Wi-Fi, Bluetooth,
  VPN and sound. Capture and phone open their existing shell destinations. The
  native header action appears only on tabs with a corresponding implemented
  destination. Legacy settings-only tabs are hidden and redirect to GTK Settings;
  settings buttons on live tool tabs launch the app. Embedded advanced editors
  have been removed. Opening Control Center without a context opens quick Home.

## Inventory and next slices

The inventory currently tracks 85 entries. No entry
is verified end to end, and `cutover` is `false`. A destination name in the
inventory states intent, not that an application or page exists.

1. Finish Control Center Home as a quick surface: Quick Settings now has labeled
   sound/brightness, everyday Wi-Fi/Bluetooth/Night Light/DND rows with stable
   titles + status captions and Settings chevrons, pin tiles from
   `control_center.shortcuts`, and labeled Lock/Suspend/Power. Hold-drag reorders
   the Quick Settings tile grid and top bar widgets. Still need Capture and Phone
   Home actions, a media strip, and keyboard/touch/multi-monitor checks under
   helix-niri. Keep detailed shell surfaces until corresponding native destinations
   work.
2. Complete existing Settings pages before marking their entries verified.
   Appearance now has roundness, borders, shadows, glass amount, icon tint color,
   and built-in and community template selection.
   Desktop & Dock now has Control Center placement, size, sidebar and session-action
   preferences, built-in page visibility, quick shortcut editing, automation hook
   commands, panel placements for launcher/clipboard/session/wallpaper/polkit/quick
   insert, panel anchor bar and floating layer, dock layout/effects/focus polish,
   per-bar layout and effects including font scale and panel overlap, dock launcher
   icon and custom image, launcher auto-paste/prefix/exchange rates, OSD
   opacity/border/margins and remaining event kinds, window-switcher order, session
   shortcut visibility, lock-screen widget visibility, screen corners, desktop-widget
   visibility and Niri layout/chrome controls, but still needs per-widget settings editors.
   Extensions are retired.
   Network needs complete connection-profile/Tailscale parity. Sound and
   Bluetooth need real device mutation, pairing and cancellation checks.
3. Add absent Settings destinations: Displays, Input/Keyboard, Devices,
   language and portable power controls. Gate desktop-only settings on the
   `helix-desktop` capability; portable pages must work without Wayland.
4. Extract Helix Software, Helix Phone, Calendar, Weather and Agent Center as
   adaptive apps. Give each an application ID, working launch target and
   portable unavailable states before moving its Control Center tab.
5. Route every launcher and IPC context to the destination, preserving
   `helix-shell` aliases and configuration paths. Only after inventory coverage and
   accessibility, installation and helix-niri checks should defaults change
   and other compositor shell code be removed.

An inventory entry becomes `verified` only when its destination opens from the
normal launcher or compatibility context, exposes the old actions and settings,
persists edits through the service, handles an unavailable backend, and passes
appropriate keyboard/touch/adaptive checks. For a shell-only surface, verify
its live behavior under helix-niri. Record any hardware or mobile check that
cannot be run. A heading, stub page, or mapped URL alone is only `partial`.

The native settings catalog is assembled in `src/desktop/settings_catalog.cpp`;
the adaptive pages and detail dialogs are under `src/apps/settings`. Retained
settings registry and helper sources remain under `src/shell/settings`;
`SettingsPane` is now only a compatibility link and renders no legacy controls.
`ControlCenterPanel::nativeDestinationForTab()` is
the current header-action map; it deliberately omits tabs whose native
destination would send users to unrelated content.

## Working safely in this checkout

There is extensive uncommitted user work in this repository. Preserve it and
review the current diff before editing shared files. The active source is under
`/home/andrewbevington/Software Projects/helix-shell`; app code is in
`src/apps`, service code in `src/desktop`, and shell controls in
`src/shell/control_center`. Do not create another project copy in Documents.

Use a focused build while this machine has memory pressure:

```sh
just build-settings
# Use just build-desktop when changing shell or System Monitor code.
nice -n 10 ionice -c 3 meson compile -C build-debug desktop_service_test -j2
TMPDIR=/tmp meson test -C build-debug --no-rebuild desktop_migration_inventory desktop_service --print-errorlogs
TMPDIR="$PWD/build-debug/helix-tmp" sh tests/install_layout_test.sh build-debug
```

`tests/desktop_migration_test.py` compares every current tab and settings
section with `desktop-migration.json` and refuses a cutover unless all entries
are verified. Add or update an entry whenever a destination changes. The full
test suite is more expensive; run it once after a larger coherent change when
the desktop has enough memory. Local screenshots and test scripts from the
Appearance work are in the Codex task's `work/` and `outputs/` folders, outside
the repository. Hardware and Linux mobile checks remain unverified; do not
silently treat private-bus simulations as those checks.

Installed binaries are in `/usr/local/bin`. After installing shell changes,
restart `helix-shell-dev.service` as directed by `.cursor/rules/helix-dev-restart.mdc`.
Reopen a running Settings window after installing its binary. Use staged
installation and checksum checks before restart.

Install only after the focused build completes: `sudo just install debug` uses
`meson install --no-rebuild`. Do not run bare `meson install` on this development
checkout: it rebuilds unrelated test targets with Ninja's default concurrency,
which has caused memory and disk pressure and desktop pauses. Installing through
`pkexec` requires an absolute build path because it changes the working directory.

## Latest local validation: 2026-10-05

- Shell, service, Settings and focused service-test targets compiled with two jobs
  at lower CPU and disk priority.
- Private-session service checks passed, including stale page lists, malformed IDs,
  legacy AI/Capture hidden aliases, preservation of hidden extension pages and reset.
- Migration inventory and staged installation checks passed. Full migration remains
  incomplete; the user explicitly authorized retiring all legacy Settings editors.
- Installed shell, service and Settings binaries match the built checksums.
- The systemd user service and development shell restarted successfully. Native
  Settings connected and enabled editing; compatibility `settings-open control-center`
  and `panel-open control-center settings:hooks` opened Desktop & Dock.
- Default Control Center open accepted the quick Home route. No service or shell
  errors were logged after restart. Current memory and disk pressure stayed low.
- Screen-reader, touch, mobile and multi-monitor checks remain unverified.

## Native quick shortcuts: 2026-10-05

Settings → Desktop & Dock → Control Center now provides add, resize, reorder,
remove and reset controls for quick shortcuts. Saved extension and unavailable
shortcut entries remain editable. New extension shortcut discovery is still pending.
Home now has one Settings header action. The service enforces typed payloads,
known catalog entries, valid sizes and stale-baseline protection, with no renderer
or plugin runtime dependency. Explicitly empty shortcut lists stay empty.

Focused builds, private-session service checks, the catalog/inventory check and
staged installation passed. Installed binaries match the build. The updated
service and shell are running; Settings connected with editing enabled on Desktop
& Dock. Hardware, touch, screen-reader and Linux mobile checks remain unverified.

### Native section navigation (2026-10-05)

Shell Settings routes now export finite `--section` destinations for Control Center, Dock, Launcher, On-Screen Indicators, Hot Corners and other hub panes. Native Settings pushes the corresponding `AdwNavigationView` sub-pane (or Privacy chip); normal page activation clears the section and search. Unknown or mismatched section requests return exit code 2. Layout inspection includes section and scroll position.

Focused shell/Settings build and desktop service/migration tests passed. Direct Control Center navigation was visually verified; page activation reset the scroll position. Narrow/mobile navigation remains unverified.

System-wide Meson installation was denied by administrator authentication. This Settings build is installed at `~/.local/bin/helix-settings`, which precedes `/usr/local/bin` in the shell service PATH. The shell dev service runs the updated repository build and was restarted. The system-wide Settings binary is older; future system-wide installation should remove or synchronize this user override.

System-wide installation retry succeeded after administrator authentication. Installed shell, service and Settings checksums match the build. Removed the matching user Settings override; `/usr/local/bin/helix-settings` is authoritative again.

### Desktop navigation polish (2026-10-05)

Desktop & Dock, Appearance, Network, System and Sound hubs use overview navigation rows that push sub-panes. Privacy & Security uses chip sub-tabs. Choice previews cover dock/bar shape, wallpaper fill, volume overdrive and brightness sync. Section activation opens the pane rather than scrolling a flat list.

Focused shell/Settings builds, migration inventory validation and whitespace checks passed. Live helix-niri layouts were inspected at 360, 768 and 1280 logical pixels; the 360-pixel layout collapses correctly and direct Control Center navigation was visually verified there. Bar navigation and ordinary page scroll reset were checked using app inspection. Physical touch, screen readers, on-screen keyboards and mobile hardware remain unverified. System-wide installation succeeded and installed Settings/shell checksums match the build.

### Appearance grouping and search (2026-10-05)

Appearance preview actions now wrap using native GTK flow layout. The normal page shows one set of library/accent actions and one theme-mode selector. Palette maintenance and wallpaper-source actions are consolidated into semantic preference groups; unavailable advanced appearance editors remain below working actions. Single-action Appearance rows activate their button when the row is clicked, following GNOME boxed-list guidance. GNOME Settings and GNOME HIG are now explicit design references in the Helix HIG.

Appearance utility actions filter with search and can be found from another page. `--search TEXT` provides single-instance search activation and can be combined with `--page`; empty search clears it. Search is rejected when combined with section or browser activation. Layout inspection now reports query and preference-group titles. Live checks covered Wallhaven/accent matches, no-match results, cross-page wallpaper search, clearing search and conflicting arguments. The focused Settings build, migration inventory test and whitespace checks passed. Layouts were checked at 360, 768 and 1280 logical pixels under helix-niri; physical mobile/touch/screen-reader checks remain unverified. System-wide installation succeeded and the installed Settings checksum matches the build. The current wallpaper file cannot be previewed by the existing image-preview loader; Settings shows its native unavailable message.

### Power and clipboard configuration (2026-10-05)

Power is now a dedicated native page covering battery warnings, suspend locking, external-display
lid behavior, fade timing and existing idle timers. System links to Power and About. Existing
power/idle/session contexts and the Control Center Power Settings link use the new page. This is
configuration coverage; live battery information, power profiles and charging-limit controls still
need service integration.

Privacy & Security now includes six Clipboard policies: history retention, preservation after
source-app exit, unpinned history limit, deletion confirmation, automatic paste mode and image
editor command. The clipboard shell surface Settings button deep-links to this group via
`--section clipboard`. Retention and current-selection preservation remain independent. Controls
are desktop-only; no clipboard content has been added to service snapshots. History contents,
clear/delete actions and encrypted-storage recovery remain in the shell clipboard surface.

Focused builds and desktop service/migration tests passed, including clipboard range/type/enum
validation, independent retention, updates/resets, Power catalogue placement and portable page
exclusion. Installed shell/service/Settings checksums match the build; services were restarted.
Power and Clipboard were visually checked at 360 logical pixels under helix-niri. Physical touch,
screen-reader, on-screen keyboard and mobile checks remain unverified. Focused just recipes now
use build-local compiler temporary directories after a /tmp quota failure.

User-requested next priorities: Displays, fuller Power coverage, and Online Accounts. Displays
should move the existing helix-niri output backend behind a versioned service interface with
validated updates and timed rollback; preserve the existing shell display controls until it works.
Online Accounts needs a portable provider/authentication integration; this machine currently has
neither GOA pkg-config development packages nor an active GOA session service. Do not expose
credential fields or fake account setup. Investigate installing GOA and embedding its supported
provider flow before implementing account management.

### Native Displays inventory (2026-10-05)

Displays is now a native adaptive page with an arrangement preview and connection, resolution,
refresh, scale, position, orientation and VRR information. `Displays1.GetSnapshot`/`Changed`
expose read-only sanitized inventory; serial numbers are excluded. The service queries niri
asynchronously on a three-second refresh cadence with a two-second query timeout. Loss of
desktop capability/backend invalidates inventory, failed reads retry, and shutdown cancels
subprocesses. Without the desktop/socket the service still starts and returns unavailable.

The page links to the existing shell Resolution & Arrangement controls; the shell display
Settings link and monitor/displays settings contexts now target the native inventory page.
Display mutation ownership has not moved: resolution, scale, arrangement, enablement, brightness
and Night Light remain in the shell. Native editing still needs validated draft/apply/revert and
recovery on service loss; no default display-editor cutover has occurred.

Focused shell/service/Settings builds and four tests passed: desktop_protocol, desktop_service,
desktop_displays and desktop_migration_inventory. The new fake-niri test covers sanitized parsing,
malformed replies, automatic recovery, query timeout and unsupported desktop state without user
hardware access. Service integration tests now explicitly clear NIRI_SOCKET as well as display
environment variables. System-wide installation succeeded and all three binary checksums match.
The live inventory matched the current built-in output (2560×1600, 240 Hz, 150% scale, VRR on).
Layouts were checked at 360, 768 and 1280 logical pixels under helix-niri. The existing shell
display surface was opened successfully through the native page's command. No real display
modes or arrangement were changed. Multi-display/hotplug, screen-reader, physical touch/mobile
and on-screen keyboard validation remain unverified. Online Accounts remains the next separate
backend integration, alongside fuller Power and native display editing.

### Clean desktop baseline and standalone applications (2026-10-05)

GNOME Online Accounts daemon/development packages were installed with explicit user approval
to cancel the previously queued offline DNF update. The update was canceled by that transaction;
it has not been rescheduled. Settings Online Accounts uses GOA >=3.56's provider-managed native
add/manage dialogs, discovers supported providers, shows accounts requiring sign-in, and recovers
when GOA restarts. GOA owns credentials/permissions/removal; no credential fields were duplicated
in Helix. Builds without GOA show an unavailable page. Provider loading, repeated page navigation
and daemon disappearance/reconnect were checked without creating accounts. Actual sign-in and
account permission/removal flows need human credentials and remain unverified. System GTK/Adwaita
development packages are now installed; Meson no longer relies on the temporary GTK SDK path.

Keyboard exposes eleven desktop-only Helix shell navigation actions using validated key-chord
arrays and UpdateOrderedList baseline checks. Invalid keys, Super modifiers, duplicates, empty
lists, stale baselines and generic Update are rejected. Reset restores configured defaults. This
does not remap GTK applications. Global Niri/workspace/application bindings still need a native
destination and safe compositor-configuration editing.

Night Light configuration moved into Displays behind a gamma-control capability explicitly
reported by the registered shell through Configuration1.RegisterCapabilities. Capability metadata
is cleared on owner loss. Unsupported edits are rejected; the native page shows an explanation.
Day/night temperature changes are committed as one pair and maintain a 100 K gap; resetting either
restores both configured temperatures. Wayland gamma application remains in the shell.

Helix Software (`org.helix.Software`, `helix-software`) is now its own adaptive application with a
graphical store UI across Flatpak, Fedora packages, and firmware, plus an `AdwNavigationView`
detail page. Flatpak Explore is a store home (search suggestion pills, featured carousel,
on-this-device strip, category rows with See all). It caches the remote catalog for client-side
search, overlays installed state off the shared package lock, and resolves icons from local
AppStream caches; Fedora Explore remains search-driven; firmware keeps Explore hidden and uses
device cards on Installed / Updates. Detail pages open installed apps, copy app IDs / install
commands, and enrich from AppStream (description, screenshot carousel, links, content rating,
release notes, Flathub verification), humanized Flatpak permissions with a safety summary
(including available apps via `remote-info -m`), `flatpak remote-info`, RPM info, and fwupd
release notes without adding libappstream. Installed/Updates support local search; update cards
show current→new versions when known and offer Remove beside Update. Mutation progress, Updates
tab badge, reviewed install/remove/update actions, Update All Listed, PackageKit/polkit
authorization, Flatpak scope handling, protected packages, batch paging and cancel→refresh
messaging remain. Dependency-resolution details and transactional failure recovery still need
refinement. Source: `src/apps/software/`.

Helix Phone (`org.helix.Phone`, `helix-phone`) is now its own adaptive application for discovery,
reviewed pairing/removal, battery state, find-phone and file sending. Current device/verification
state is rechecked before pairing and capability/reachability before file sending. Unsupported and
offline actions show explanations. Its PhoneConnectService client mode does not arm AirPlay,
change ANCS/notification capture or write the shell-owned transfer history; it uses a separate
discovery token. The first staging run inherited automatic AirPlay behavior; it was stopped and
client-only behavior was implemented and verified against the paired device without those side
effects. Mirroring, casting, synchronization, received-file/history views and advanced phone
configuration remain in the existing shell surfaces until replacements work. No pairing, ring,
file transfer or device removal was performed for validation. Source: src/apps/phone/main.cpp.
Both applications use shared toolkit-role appearance binding with native fallback. Settings System
links to both apps, keeping package/phone tools out of Settings.

Retired duplicate Software panel: packages/software contexts now open Helix Software; legacy
helix aliases and config paths remain. Removed packages_tab.cpp/.h, HostedEditors sources/header
and SettingsSheet/SettingsSheetModal sources/headers from the repository and build. Modified
source was backed up outside the repository under outputs/retired-desktop-editors. Other live
advanced phone/display/device surfaces remain; no full desktop cutover has occurred.

A fresh build of shell, service, Settings, System Monitor, Software and Phone completed in
build-native-clean with low priority/two jobs and build-local compiler temporary files. All six
were installed with --no-rebuild and checksum-verified. System Monitor's installed desktop entry
and resource snapshot were explicitly verified. Previous build-debug was archived as
build-debug-pre-native-clean; build-debug is now a symlink to build-native-clean so existing just
recipes and helix-shell-dev.service run the clean baseline. No source edits were reset. Service
and shell were restarted and the systemd user manager reloaded.

Five clean-build tests passed: desktop_service, desktop_protocol, desktop_displays,
desktop_migration_inventory and package_spec. Staged install_layout checks passed and now require
Software and Phone binaries/desktop entries. Native app reads/layouts were checked at narrow
widths; physical touch/mobile, OSK and screen-reader checks remain unverified. Layout inspection
now returns an error without an open window rather than starting a new primary GUI and hanging.

Migration remains incomplete (cutover=false). Settings catalog now covers Capture,
location/weather, appearance backdrop/shell-mode/wallpaper automation scalars, sound
overdrive/theme, brightness, network/bluetooth preferences, clock/calendar formats,
Tailscale/LocalSend, privacy filters and session power overrides. Complex editors
(per-widget, launcher providers/dmenu, plugins, per-monitor wallpaper pickers) remain
stubbed. Next work includes safe native display mutation/rollback and single backend
ownership, fuller Power/global Niri/input settings, widget/template/icon editors,
security/device destinations, native phone casting/sync, and dedicated Calendar/Weather/
Agent Center applications. Keep inventory statuses honest; do not mark pending
destinations verified merely because old editors were retired.
