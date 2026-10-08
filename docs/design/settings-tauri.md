# Settings, rebuilt as a Rust + Tauri application

Status: started 2026-10-08. Built: the window, the Privacy & agents page (manager role: who holds which tiers, switch a tier, revoke, the audit), the manager identity check
in the service, and the shared design tokens. Everything else below is still plan.

Try it: `tools/switch-shell.sh tauri` links it as `helix-settings-new` (the installed C++ Settings is untouched).

## Why rebuild it, and what not to carry over

The current Settings app (`helix-settings`, C++, `src/shell/settings/`) is the settings UI of the *native* shell: the catalog behind it
(`src/desktop/settings_catalog.cpp`, ~380 entries) is dominated by that shell's own surfaces: 172 entries for its bar and dock indicators,
60 for its appearance engine, 41 privacy/security (including the agent pages). Most of the bar/dock/control-center entries describe
things the Tauri shell does not have or does differently. So this is not a port of the page list. It is a new app with the pages the Helix
desktop needs, reading the service's catalog only where the service still owns the setting (theme mode, night light, notification enable,
key chords).

## What it is

- A separate Tauri 2 application, one window (a normal toplevel, not a layer surface), its own process, started on demand and gone when
  closed, so it costs nothing while closed (the shell's budget is for the shell).
- Installed as `helix-settings` (replacing the C++ binary) because the service recognises the Agent1 *manager* by that executable name.
  **That check must be tightened first** (`src/desktop/agent_endpoint.cpp:271` compares only the file name: any file called
  `helix-settings` qualifies). It should compare the canonical path with the installed locations (`/usr/local/bin`, `/usr/bin`), with a
  test-only environment override.
- Started by `helix-panel settings` (and the shell's Settings entries); a second start asks the running one to show a page
  (`org.helix.Settings1.ShowPage(page)` on the session bus) instead of opening another window.
- Shares, not copies: the design tokens and icons (`rust/helix-shell-tauri/dist/style.css`, `icons.js` move to a shared directory both apps
  take at build time), the pure models in `helix-shell-core` (audio, network, bluetooth, brightness, quick settings, dock, notifications) and the
  D-Bus client `helix-desktop-api`. A page is a larger view of a model the shell already tests.

## Pages (first release, in the order they are built)

1. **General / Appearance**: theme mode (dark/light/auto), accent colour, wallpaper (image or colour, per monitor), text size.
   Writes `Configuration1.Update` for service-owned settings and the wallpaper settings file for the wallpaper process.
2. **Displays**: resolution, refresh rate, scale (fractional), arrangement, night light schedule. Needs `Displays1`.
3. **Sound**, **Network**, **Bluetooth**: the control center's models with full lists, details, and the prompt flows the control center
   cannot do (Wi-Fi password, Bluetooth pairing, VPN).
4. **Notifications**: do not disturb (and a schedule), per-application rules, history length.
5. **Keyboard & shortcuts**: the key chords (`keybinds.*`), the niri binds the shell uses (shown, with a conflict check), Caps Lock behaviour.
6. **Dock & bar**: pinned applications, what the bar shows, auto-hide, magnification.
7. **Power & lock**: idle times, what happens on lid close, the lock command (`idle.toml`, validated by `helix-idle`'s own rules).
8. **Privacy & agents**: the Agent1 manager role: which applications have which tiers, revoke, the audit, the kill switch, and the
   consent each agent was given. This is the page that matters most for an agentic OS.
9. **About**: versions, the install, the model, diagnostics (the budgets: memory, CPU).

## How a page is built (same rules as the shell)

1. A pure, mutation-tested model in `helix-shell-core` (or a new `helix-settings-core`) turns what the service says into a view and is the
   *only* producer of write requests (ranges, ids that must come from the last read, what is refused and why).
2. A thin Tauri command sends the request; a watcher follows `Changed` signals; never poll.
3. The page draws what Rust sends, text only through `textContent`, strict CSP, named controls (`data-test`).
4. A nested end-to-end test drives it with a real pointer against `fake-desktop.py` (extended per page) and asserts the exact requests
   the service received, including those that must *not* be sent.

## Capabilities the app needs (and no more)

Tauri's capability file for the `settings` window: the core defaults, window drag/close, and one file chooser for the wallpaper (a
portal call or a dialog plugin, decided when that page is built). No shell, no arbitrary filesystem, no HTTP. The app reads and writes only:
the D-Bus services it lists (`org.helix.Desktop.*`, `org.helix.Shell` for panel actions), its own config files under
`~/.config/helix/` (atomic writes, 0600, validated models), and the wallpaper and idle files. Agent1: *manager* role only (never client or
confirmer). A boundary test, like the shell's, fails the build if the app's sources name anything outside that list.

## Order of work

1. Tighten the manager identity check in the service (+ test), keeping a test override.
2. Shared UI directory (tokens, icons) consumed by both apps; the shell's tests must still pass.
3. `rust/helix-settings`: window, sidebar, page router, the **Privacy & agents** page first (it is the one with real new behaviour and a
   security model), then Appearance, Sound/Network/Bluetooth, the rest.
4. Install through `tools/install-assistant.sh` (the person types sudo), rename out the C++ `helix-settings` only when the new app
   covers its pages that still matter; keep `switch-shell.sh native` working until then.
