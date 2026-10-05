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
of the replaced pane are saved in the Codex task's `outputs/retired-settings-source`.

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

The inventory currently tracks 84 entries: 26 partial and 58 pending. No entry
is verified end to end, and `cutover` is `false`. A destination name in the
inventory states intent, not that an application or page exists.

1. Finish Control Center Home as a quick surface: check the new capture and
   phone entries with keyboard, touch and multi-monitor behavior under
   helix-niri. Add explicit backend-unavailable states where the existing
   shell destinations need them. Keep detailed shell surfaces until corresponding
   native destinations work.
2. Complete existing Settings pages before marking their entries verified.
   Appearance still needs icon and template catalogs and advanced editors; the
   retired shell editors are no longer available as a fallback.
   Desktop & Dock now has Control Center placement, size, sidebar and session-action
   preferences, built-in page visibility and quick shortcut editing, but still needs widget, extension and
   hook editors.
   Network needs complete connection-profile/Tailscale parity. Sound and
   Bluetooth need real device mutation, pairing and cancellation checks.
3. Add absent Settings destinations: Displays, Input/Keyboard, Devices,
   language and portable power controls. Gate desktop-only settings on the
   `helix-desktop` capability; portable pages must work without Wayland.
4. Extract Helix Software, Helix Phone, Calendar, Weather and Agent Center as
   adaptive apps. Give each an application ID, working launch target and
   portable unavailable states before moving its Control Center tab.
5. Route every launcher and IPC context to the destination, preserving
   `noctalia` aliases and configuration paths. Only after inventory coverage and
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

Shell Settings routes now export finite `--section` destinations for Control Center, Dock, Launcher, On-Screen Indicators and Hot Corners. Native Settings scrolls to the corresponding group while keeping the complete page; normal page activation clears the section and search. Unknown or mismatched section requests return exit code 2. Layout inspection includes section and scroll position.

Focused shell/Settings build and desktop service/migration tests passed. Direct Control Center navigation was visually verified; page activation reset the scroll position. Narrow/mobile navigation remains unverified.

System-wide Meson installation was denied by administrator authentication. This Settings build is installed at `~/.local/bin/helix-settings`, which precedes `/usr/local/bin` in the shell service PATH. The shell dev service runs the updated repository build and was restarted. The system-wide Settings binary is older; future system-wide installation should remove or synchronize this user override.

System-wide installation retry succeeded after administrator authentication. Installed shell, service and Settings checksums match the build. Removed the matching user Settings override; `/usr/local/bin/helix-settings` is authoritative again.
