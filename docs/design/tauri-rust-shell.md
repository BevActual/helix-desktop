# Tauri/Rust shell: direction, evidence and plan

Decision (owner): build the Helix shell and apps on Rust and Tauri, replacing the C++ shell surface by surface. The C++
shell keeps running until each replacement is at parity. This document records what is known, so work starts from facts.

## Evidence from the spike (`rust/helix-shell-tauri`)

- A Tauri 2 window becomes a layer-shell surface with `gtk-layer-shell`. Verified in a nested niri: the bar is a Top-layer,
  full-width surface that reserves space, no keyboard; the panel is an Overlay-layer surface with exclusive keyboard.
  Both paint correctly (pixel coverage bar 83.5%, panel 44.6%, `tools/nested/coverage.py`).
- Gotchas that cost time. Do not rediscover them:
  - Layer surfaces take their size from the GTK widget: call `set_size_request` before `init_layer_shell()`. Without it the
    webview is mapped but paints (almost) nothing.
  - `WEBKIT_DISABLE_DMABUF_RENDERER`, `WEBKIT_DISABLE_COMPOSITING_MODE` and software GL all made rendering WORSE here
    (blank). Do not set them. The earlier "blank on NVIDIA" conclusion was wrong; the cause was missing size requests.
  - `gtk-layer-shell` needs the `v0_6` feature for `set_keyboard_mode`. The Tauri build needs `icons/icon.png`.
  - Create the windows hidden, run `init_layer_shell()` before showing.
- Measured cost (PSS, 4 runs): bar window with a hidden panel 227-280 MB; bar plus visible panel 231-336 MB (4 processes,
  two WebKit web processes); idle CPU about 0.07%; first frame about 400 ms. The native C++ shell is 105 MB for everything.
  So the web shell costs roughly 2-3x the memory. This is accepted; the work is to keep it from growing (see budgets).
- Tauri windows are GTK3 + WebKitGTK 4.1 (`webkit2gtk4.1-devel`, `gtk3-devel`, `gtk-layer-shell-devel`, `libsoup3-devel`).

## Principles carried over from the C++ work (they are requirements, not preferences)

1. **The contracts are the boundary.** D-Bus interfaces in `protocols/dbus/*.xml` and the msg command schema stay the single
   source of truth. The Rust code generates or checks against them. Do not invent parallel APIs.
2. **The model's authority never lives in the shell.** The assistant stays a separate process (`helix-assistant`) with its
   own Agent1 identity and grants. The shell UI only shows its JSON (`--json`, `--execute`). A shell bug must not widen
   what any agent may do. Do not give the shell binary Agent1 grants.
3. **Approval surfaces are not spoofable by content.** The Agent1 approval dialog, the lock screen and the greeter must
   render only trusted, escaped text, never HTML from a model, app or notification. All external text is set as text
   (`textContent`), never as markup. Strict CSP, no remote content, no `eval`, minimal Tauri capabilities, each Tauri
   command takes validated typed arguments.
4. **Enter never approves; approval is armed after a delay (700 ms); closing/escaping denies.** Port the behaviour and its tests.
5. **Tests prove behaviour, and are mutation-checked:** break the code, confirm the test fails. Unit-test state machines
   with no UI in them (see `src/shell/assistant/assistant_session.*` and its test).
6. **Layering ratchet.** Pure logic in plain crates with no Tauri dependency; the Tauri crate is a thin face.

## Target layout (grow into it; do not create empty crates in advance)

```
rust/
  Cargo.toml                       workspace
  helix-desktop-api/               zbus client for org.helix.Desktop (+ Inference), types from protocols/dbus/*.xml
  helix-shell-core/                pure logic: panel state machines, assistant session, confirm queue (no Tauri, no GTK)
  helix-shell-tauri/               the shell app: layer-shell surfaces (bar, panels), commands, frontend in dist/ or ui/
```

## Milestones (each ends with: tests, a nested-session screenshot, and the budget table below)

- **M0 done.** Bar + panel spike renders in nested niri.
- **M1 Foundation.** Workspace builds in CI-like conditions (`cargo test`, `cargo clippy -D warnings`); `helix-desktop-api`
  talks to the real `helix-service` in a private bus (`tools/nested/bus.conf`); one live value (volume or clock-with-config)
  flows service -> Rust -> webview and updates.
- **M2 Surface manager.** One Rust module owns every layer surface: create hidden, size, anchor, layer, keyboard mode,
  exclusive zone, show/hide, dismiss on Escape and outside click, one panel open at a time, per-monitor. Unit-test the
  state machine; screenshot-test with `tools/nested/`.
- **M3 Bar.** Real widgets driven by the existing config and the desktop service (clock, workspaces via niri IPC,
  volume, network, battery, tray later). Visual parity with the native bar on the same wallpaper.
- **M4 Assistant panel parity.** Same states and rules as the native one (Thinking, confirm with Do it/Cancel, 700 ms arming,
  Enter never confirms, stale answers dropped, display-safe text, focus back to the input). Port `assistant_session` tests.
- **M5 Launcher, control center, notifications, OSD.** One at a time, each behind a config flag so the C++ surface remains
  the default until parity is signed off.
- **M6 Decide lock screen and greeter.** They are trusted surfaces (ext-session-lock, greetd). Decide with measurements and
  a threat review before moving them; they may stay native.

## Budgets (record every milestone; do not let them drift)

| Metric | Native C++ shell today | Tauri spike | Budget |
|---|---|---|---|
| Whole shell PSS | 105 MB | 227-336 MB (bar + panel only) | <= 350 MB with bar, one panel, notifications, OSD |
| Idle CPU (bar only) | not measured | ~0.07% | <= 0.3% |
| First frame | not measured | ~400 ms | <= 800 ms |
| Frontend bundle | n/a | ~10 KB | <= 150 KB gzipped |

Measure with `tools/nested/run.sh` and `tools/nested/process_tree.py` (sums PSS across WebKit helper processes).
If a milestone blows a budget, stop and report; do not push on.

## How to work

- Everything runs in the nested session, never the real one. Never `pkill -f`; stop processes by PID. Do not install files
  into `/usr/local` or restart the user's shell; installation is a separate, explicit step.
- Packages already installed for this: `webkit2gtk4.1-devel gtk3-devel gtk-layer-shell-devel libsoup3-devel javascriptcoregtk4.1-devel`.
- Commit small, one topic each, with the repo's message style. Update this document when a fact changes.
