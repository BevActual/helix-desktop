# Desktop service and adaptive Settings

Product placement: `product-structure.md`. Cutover: `cpp-to-rust.md`.

Helix Shell retains its renderer. Desktop authority is a separate session process:
live **C++ `helix-service`**, target **Rust `helix-desktop`**. `helix-settings` is
an independent Tauri application (`apps/helix-settings`). The shell uses the
desktop API for configuration writes, appearance, NetworkManager, BlueZ and audio
control. The service owns device backends long-term; PipeWire observation may
stay local where a surface still needs it until that domain cuts over.

## Versioned session API

Bus name: `org.helix.Desktop`. Object: `/org/helix/Desktop`.
Interfaces: `Configuration1`, `Appearance1`, `Audio1`, `Network1`, `Bluetooth1`,
`Displays1`, `AgentSurface1`, `AgentSurfaceActivity1` (full prefix `org.helix.Desktop.*`).
**`protocols/dbus/org.helix.Desktop.xml` is authoritative for wire types.**

Every domain provides `GetSnapshot() → a{sv}` and `Changed` notifications.
A capability is available only when its snapshot says so. Configuration reports
`helix-desktop`, `audio`, `network`, and `bluetooth`. Missing device backends
return readable unavailable snapshots rather than preventing navigation.

`Configuration1.GetSettings() → av` returns renderer-independent dictionaries:
`id`, `page`, `title`, `group`, `type`, `value`, `options`, `minimum`, `maximum`,
and `overridden`. Optional `option-labels` provide display text while `options`
retain stable wire values. `Update(s,v)` validates supported options, types, ranges and
existing configuration rules. `Reset(s)` removes the override. Desktop-only
metadata is omitted when no shell is registered. Dynamic bar IDs use
`bar:<name>:<field>`; names containing periods remain intact.

The shell calls `RegisterDesktop()` and retains that D-Bus connection. Only its
sender may call `CommitOverrides(s baseline,s candidate)` or access runtime
state. A stale baseline is rejected rather than overwriting a concurrent edit.
Existing config paths, precedence, migrations and environment overrides remain
handled by ConfigService. External changes are reloaded by the service and shell.

Audio, Network and Bluetooth accept `Action(s,a{sv}) → t`. The returned operation
ID is acceptance, not completion. `Cancel(t)` accepts only the requesting sender and
only operations with backend cancellation support (Wi-Fi connection and Bluetooth pairing). `Completed(t,b,s)` goes to the requesting
connection after the target state is reached or the deadline expires. Network
and Bluetooth send `Prompt(a{sv})` only to the owner of the authentication
operation. That same sender must answer `Reply(s,b cancel)`; disconnecting the
client cancels its outstanding authentication prompts. Secrets are excluded from
snapshots. Backend exceptions use fixed public errors to avoid exposing inputs.
Invalid values return `org.helix.Desktop.Error.InvalidArgument`; failed operations
return `org.helix.Desktop.Error.Failed`.

Appearance generation is portable. `RefreshPalettes()` refreshes the existing
community catalog through the service; palette metadata lists only available
community/custom choices. `SavePalette("current")` persists a named custom palette. Desktop-wide template exports and GSettings
updates require a registered Helix shell. Settings exports supported semantic CSS
color variables only, keeps native control geometry, and uses native defaults
when unavailable or in high contrast. Whole desktop-only pages are omitted.

`Appearance1.GetSnapshot()` also provides `palette-previews`: built-in and
available community/custom palettes with source, name, and compact light/dark
surface and accent samples. Settings uses these samples in a searchable native
palette browser. Browsing and switching the preview variant do not change the
desktop; Apply writes the selected palette and source through the service's
validated configuration updates. The gallery scrolls inside an adaptive dialog,
and it retains an unavailable state if the service disconnects.

Missing backends are retried every three seconds. NetworkManager/BlueZ unique-name
changes invalidate pending operations and rebuild their backend/agent together, even
when a daemon restarts between checks. PipeWire connection errors release meters and
pending operations, then reconnect the service mixer independently.

`Audio1.StartInputMeter(u source)` starts or renews an explicit eight-second microphone
test lease for the calling connection. `StopInputMeter()` stops that caller's capture.
`InputLevel(u source,d peak,b available)` is sent only to that caller. Samples are
reduced to a bounded peak and immediately returned to PipeWire; nothing is saved.
Settings renews the lease only while its Sound page test is on (`Test microphone`) and stops it on leaving that page.
Disconnecting a client releases its stream. Level updates never enter snapshots.

`Network1.GetDns()` reads the active connection profile. `SetDns(a{sv})` accepts
Automatic, Google, Cloudflare, NextDNS or custom IPv4/IPv6 servers, with optional
TLS server names. The service rejects unsupported profiles, malformed types,
invalid addresses and invalid NextDNS IDs before invoking NetworkManager. Settings
provides a native DNS editor and a live, selectable connection-details view.

Wallpaper metadata includes fit mode, native folder selection, transition timing,
rotation interval/source/order, daily provider and video playback preferences.
These and shell icon tinting are desktop-only. Nonempty folder updates must name
an existing absolute local directory; reset restores configuration precedence.
The native wallpaper browser covers local image libraries and Wallhaven search,
download and application through the service. Per-monitor wallpaper overrides
and other provider catalogs remain separate migration work.

Desktop & Dock exposes running indicators/window badges, active-display behavior,
reserved space, launcher-button placement, magnification, spacing, monitors, layer,
shadow, background opacity, corner radius and focused/unfocused icon scale and opacity.
Launcher preferences include categories, grid mode, application actions, source indicators,
auto-paste after copy actions, provider prefix and currency rate fetching.
On-screen indicators include position, scale, opacity, border, margins, displays and
per-event toggles. Window switcher order and session shortcut visibility are also exposed.
Control Center preferences include its sidebar mode, direct-detail sidebar mode,
width, height, attached/floating placement, floating position, pointer-relative
opening and Home session-action visibility. Panel general settings cover the
anchor bar and floating panel layer. All use existing configuration
paths and service validation. Built-in page visibility and quick shortcuts now
have native editors in Settings.
Each named bar exposes reserved space, workspace-switch reveal, layer, shadow,
contact shadow, thickness, content scale, font scale, panel overlap, padding, widget spacing, margins,
background opacity and corner radius alongside visibility, position and auto-hide.
Dock launcher chrome includes icon name, optional custom image and colorize.
Lock-screen widget visibility is available on Privacy & Security.
These preferences remain desktop-only where noted; per-widget property editors still need migration.
Extensions are retired.

Dock and launcher favorites use ordered `application-list` metadata.
`Configuration1.UpdateApplicationList(s id,av baseline,av candidate)` rejects stale
baselines, duplicate/malformed IDs and lists longer than 256 entries. Settings
provides an alphabetically sorted, searchable installed-application picker and
visible reorder/remove/reset actions. Missing applications remain in the list
so installing them again restores their position.

Each named bar provides ordered `widget-list` metadata for its start, center and end
sections. Options include configured built-in and plugin widget instances.
`UpdateOrderedList(s id,av baseline,av candidate)` supports favorites and bar sections;
`UpdateApplicationList` remains compatible for application favorites. Unknown new
widget IDs, duplicate entries and stale baselines are rejected. Existing unavailable
instances remain editable so their positions survive plugin removal/reinstallation.
Native visible menus provide reorder/remove actions; reset preserves config precedence.

Notifications exposes native popup visibility/actions, position/scale/margins,
visible-count and dismissed/history retention preferences. Zero limits preserve the
existing automatic/unlimited behavior. Accessibility collects Helix contrast, scale
and motion settings. Both pages require a registered Helix desktop and disappear
on other desktops; they do not manage another desktop's notification server.
About remains portable and shows OS, kernel, architecture and backend availability
from Configuration snapshots. It excludes hostnames, credentials and identifiers.
Per-application notification filters and broader accessibility integration remain
pending. The user explicitly retired the legacy Settings editors on 2026-10-05;
existing saved configuration remains active while native replacements are built.

Notification display targeting uses `display-list` metadata with native add/remove/reset
controls. Selectors are preserved even when an output is disconnected; an empty list
means all displays. `UpdateOrderedList` also validates these lists and rejects stale
saves. Notifications adds layer, opacity and outline preferences for shell popups.
Desktop & Dock includes indicator placement/orientation, display selectors, scale,
master enablement and event visibility. These preferences target shell surfaces;
application controls retain native geometry and solid styling. Live multi-monitor
routing remains an unverified hardware check.

`control_center.visible_tabs` and `control_center.shortcuts` remain in the settings
catalog and schema for config compatibility (and nested config smoke). The GTK
Control Center / Settings editors that owned those UX surfaces are gone; Tauri
Quick Control Center and Settings do not read these rows yet. Prune or re-home
them only after an explicit product decision (see `desktop-contract-tests.md`).

Notification rules are exposed as ordered dictionaries under `notification.filter`.
Settings supports native add/edit/disable/reorder/remove/reset actions with application
or category tokens and optional content expressions, popup/history/sound preferences,
DND bypass and persistent-notification policy. The first matching enabled rule wins.
The service validates unique names, typed flags, expression syntax, urgencies and
32-bit timeout values; ordered saves reject stale baselines. Urgency gates and optional duration overrides have native controls; zero duration
requests persistence and -1 uses the server default. Rules retain the existing configuration serializer and filter order. Whole-list
writes set `notification.filter_replace=true`; this explicitly replaces base-file
rules, including an empty list. The default false preserves previous named-rule
merge behavior. Reset removes the rule table, ordering and replacement marker
together to restore configuration precedence.

System exposes Helix battery-warning threshold, suspend locking, external-display
lid behavior and idle fade preferences. Each configured idle timer exposes enablement,
normal/locked delay and built-in action selection. Existing custom commands and resume
commands are preserved; new arbitrary commands are not edited by these controls.
Timer updates re-read current configuration and write the existing whole-collection
format atomically. Reset restores the entire configured timer collection and order.
System remains hidden without a registered Helix desktop. Portable power-profile,
battery-state and language controls remain pending, as do live suspend/lid checks.

## Development and validation

Build with `cargo build --profile fast -p helix-desktop -p helix-settings` (`BUILDING.md`). Installed builds use D-Bus activation and the
systemd user unit. Run `helix-settings --page appearance` (also `wallpaper`, `accessibility`, `displays`, `network`, `bluetooth`, `sound`,
`keyboard`, `dock`, `power`, `agents`, `about`). The application is single-instance. Search filters setting metadata across the pages.

Protocol and service tests run on a private session bus without real hardware.
The service test checks missing backends, strict validation, persistence, restart,
external edits, desktop capability gating, bar preferences and stale commits.
An optional private PipeWire integration test uses a silent virtual source,
verifying live levels, explicit stop, lease expiry, client loss, startup recovery and
daemon restart without inspecting real hardware. Historical GTK tab routing is archived in
`docs/design/archive/desktop-migration.json` (no gate). The initial five pages do not replace Control Center.

## Remaining migration work

The first five pages are an initial implementation, not complete Settings parity.
Icon and template catalogs, per-monitor wallpaper editors, advanced appearance
editors, connection-profile editors and further detailed network controls still
need migration. The local and Wallhaven wallpaper browser is available.
Builtin, cached/refreshed community and custom palette selection plus saving the
current palette are available, as are wired/cellular and existing VPN controls.
Microphone level metering, connection/pairing cancellation and backend
recovery are now implemented; real audio/microphone hardware validation is pending.
Other Settings destinations and extracted applications
are still pending in the inventory. The quick-controls-only Control Center,
removal of other compositor integrations, and full desktop cutover have not happened.
The legacy in-shell Settings editors were retired on 2026-10-05; Settings entry points
launch Tauri `helix-settings` (`apps/helix-settings`). The session shell is Tauri
(`shell/helix-shell-tauri`); Control Center and Notification Center are shell surfaces,
not a GNOME status area.
Advanced editors without native replacements are unavailable and identified in the app.
Real Wi-Fi/pairing mutation tests, screen readers, touch/on-screen keyboards,
non-Helix desktop and Linux mobile hardware, and multi-monitor coverage must be
completed before cutover. Subsequent Helix Link/Agent Center/Project Spaces work
is also outside this completed foundation.

### Native Settings entry points

Control Center offers a visible native-app action in its content header only when
that tab has an implemented destination. It opens Settings for audio, network,
Bluetooth, appearance, notifications, privacy, desktop preferences and available
system controls, or System Monitor for overview and screen time. It dismisses
the panel after launching. Tabs without a matching destination retain their
existing editor and have no misleading native-app action. The installed
desktop entry also exposes Appearance, Sound, Network and Bluetooth actions to launchers
and docks supporting desktop actions. Settings activation keeps one application instance.

### Privacy & Security coverage

The desktop-only native page covers shell HTTP restrictions, external IP lookup, startup
telemetry, password mask presentation, fingerprint preference, lock-screen blur/tint and
interactive display selection. Descriptions distinguish preferences from available hardware
and explain that shell offline mode does not restrict other applications. Location, weather,
privacy-indicator filters, greeter sync and credential-storage preferences are now catalogued
on Privacy & Security. Firewall and advanced authentication management still use their
existing destinations; migration remains partial.

### Hot corners and connection recovery

Desktop & Dock exposes hot-corner enablement, hold delay (0–2000 ms), each corner’s
action and custom command. Commands use the existing shell backend and are only run
when their corner triggers; saving never executes them. Resetting one field preserves
its sibling action or command. These controls require an active Helix desktop.

After an unsuccessful service refresh, Settings preserves the last known desktop
navigation and selected page, disables edits and offers reconnection. A successful
portable-desktop snapshot still hides Helix-only pages. The layout diagnostic also
reports whether mutations are enabled.

### Independent System Monitor

`helix-system-monitor` (`org.helix.SystemMonitor`) provides adaptive Overview and
Processes pages. Overview samples Linux CPU counters, available memory, swap, aggregate
network throughput excluding loopback, root filesystem usage and uptime. Processes are
searchable by name or PID and sortable by memory, CPU or name; selecting a row shows
PID, state, resident memory, thread count and sampled CPU usage. Process CPU uses one
core as 100% and may exceed 100%. Sampling is asynchronous and can be paused with Live.
Permission-restricted or disappearing processes are omitted; unavailable counters are
explicitly reported. No process command lines, environments, credentials or history
are collected, and process control is not implemented in this milestone.

The application needs neither the shell nor desktop service. If an appearance service
is already running, it consumes semantic roles through the shared application mapping;
it never activates the service. Native appearance is restored when that service is absent.
`--page overview|processes|screen-time`, `--search QUERY`, `--inspect-layout` and headless `--snapshot`
are supported. Launcher desktop actions expose Processes. Control Center System and
Screen Time contexts offer Open System Monitor, while their current surfaces remain
available. GPU metrics, per-interface/per-core charts and advanced screen-time reporting still
need migration; the corresponding inventory entries remain partial.

The Screen Time page reads existing saved daily, per-application and hourly usage
without creating or modifying history. It honors `HELIX_STATE_HOME` and XDG state
paths through the shared path resolver. Historical window-title suffixes are stripped
and combined by app ID. Missing, malformed or restricted files have an explicit state;
a temporarily unreadable file preserves the previously loaded report and retries.
The reader bounds file size, dates, application counts and durations. Reports refresh
every 10 seconds while that page is open and Live is enabled. Native Settings exposes
the existing opt-in recorder toggle under Privacy & Security. Live unsaved history,
week summaries, charts, history clearing and a portable recorder remain outstanding.

### Graphical application content

Overview includes native-color CPU and memory timelines for up to two minutes. Pausing
Live drops in-flight samples, and gaps are kept separate rather than interpolating
through missing activity. Screen Time provides an hourly histogram and per-application
usage bars, with exact hourly values in a native expandable section. Chart captions
wrap on narrow screens and every chart has a textual counterpart and accessible label.
Settings Appearance shows semantic palette swatches and a complete wallpaper thumbnail
with nearby native Accent and Wallpaper actions. Thumbnail decoding is asynchronous,
bounded to 64 MiB files/32 megapixels, applies embedded orientation, and reports an
unavailable preview for unsupported files. The service exposes the selected wallpaper
path only with the Helix desktop capability. Applications share one GTK appearance
mapping; controls keep native geometry. `--save-preview FILE` exports a running native
window for visual verification.

## Native wallpaper browser and Wallhaven

Settings → Appearance → Wallpaper opens an adaptive native dialog with My Images
and Wallhaven sources. My Images scans the selected local folder off the UI thread,
provides filename search, optional subfolders, and pages of 24 thumbnails. Scans omit
hidden files and symlinks and stop at 1,000 images or 8,192 entries. A separate file
chooser retains access to other wallpaper formats supported by the shell.

Wallhaven supports text search, categories, purity, sorting/order, resolution and
aspect-ratio filters, paging, thumbnails, cancellation and Download & Apply. Search
is explicit. Existing wallpaper.wallhaven settings take precedence over the legacy
helix/wallhaven plugin settings. API keys stay inside the service; snapshots expose
only their availability. The native password dialog can save a replacement key. The
download folder is an ordinary validated folder setting.

Appearance1 adds SearchWallhaven(a{sv}) → t, GetWallhaven(t) → a{sv},
DownloadWallhaven(t, s) → t, CancelWallhaven(t), and SetWallhavenApiKey(s).
Jobs belong to their D-Bus caller, expire after five minutes without polling, and
are cancelled on caller loss, desktop capability loss or offline mode. Results
contain busy/error state, image IDs, resolutions and local thumbnail paths. Each
client may hold four jobs, with sixteen across the service. Queries reuse the old
Wallhaven URL builder and parser; only trusted HTTPS image hosts and validated
image IDs can become downloads. Credentials use the API header, without redirects.
Search bodies, thumbnails and full images have separate size limits. Files are
written through unique temporary files and renamed only on successful completion.
The service applies the wallpaper only after the full image download succeeds.

The wallpaper migration remains partial: favorites still need a native destination.
Settings picks a picture for every display or for one connected display, and
`helix-wallpaper` paints that choice. Daily provider, video wallpaper, fill color, transitions and
automation scalars are available in Appearance. Legacy Settings editors have been
retired; full desktop migration remains incomplete.

Launch the browser with `helix-settings --wallpapers library` or
`helix-settings --wallpapers wallhaven`; both reuse the existing Settings instance.

## Graphical Settings previews

Appearance offers native illustrated Light, Dark and Automatic toggles using the
current palette. They share the authoritative theme.mode setting with the normal
preference row and stay synchronized with external configuration changes.
Desktop & Dock and Notifications keep responsive layout illustrations on the hub
overview. Hub sub-panes add choice illustrations for dock/bar shape, wallpaper fill,
volume overdrive and brightness sync (`choice_preview.h`). These are explicitly
illustrative rather than exact multi-display renderings. Diagrams update in place,
retaining native control focus, and have textual captions for accessibility. Native
contrast colors remain authoritative.

Settings hubs use `AdwNavigationView`: overview root plus pushed sub-panes for Desktop &
Dock, Appearance, Network and System; Privacy & Security uses chip sub-tabs; Sound keeps
Sounds inline under the live mixer. `--section` opens the matching pane (or Privacy chip),
not only scroll position:
`control-center`, `dock`, `bar`, `launcher`, `osd`, `hot-corners`, `window-switcher`,
`capture`, `screenshot`, `screen-record`, `hooks`, `panels`, `niri`, `desktop`/`widgets`
and `media` on Desktop & Dock;
`colors`, `type`, `roundness`, `glass`, `icons`, `effects`, `templates`, `wallpaper`/`theme`,
`video-wallpaper` and `backdrop` on Appearance; `wifi-prefs`, `dns` and `tailscale` on
Network; `clock`, `calendar`, `language`, `setup` and `advanced` on System;
`sounds` on Sound; `clipboard`, `privacy`/`firewall`/`security`, `lock-screen`, `location`,
`authentication` and `storage` on Privacy & Security; `scale` on Accessibility. It may be
combined with the matching `--page`; other page combinations and unknown sections are
rejected. Shell compatibility contexts use these destinations when available.

Settings accepts `--search TEXT` for single-instance search activation, optionally with `--page ID`.
An empty search clears the query. Search is incompatible with section or browser activation.
`--inspect-layout` includes the active query and displayed preference-group titles.

The Power page owns Helix battery-warning, suspend/lid and idle-timer configuration. System retains
navigation to Power and About. Power, idle and session compatibility contexts target Power.
Privacy & Security exposes desktop-only clipboard retention, current-selection preservation, history
limit, deletion confirmation, automatic paste and image-command policy; clipboard contents and
encrypted-storage recovery remain in the dedicated shell surface. `--section clipboard` navigates
to that group on Privacy & Security, including when launched from the clipboard surface.

`org.helix.Desktop.Displays1` provides `GetSnapshot` and `Changed` for a read-only helix-niri
display inventory. Snapshots contain availability, capability IDs (`inventory`), and sanitized
output records with connector/name, enabled state, mode, logical size/position, scale, transform
and VRR state. Device serial numbers are not exported. Querying requires a registered Helix
desktop and compositor socket; it is asynchronous, bounded to two seconds, retries automatically
and clears stale outputs when unavailable. No display mutations are exposed through this interface.
Settings changes resolution, scale, orientation, arrangement, variable refresh, and power on the
compositor socket (`Request::Output`) and records them in `helix-setup.kdl`. Night light stays off
this page until a gamma client exists.
