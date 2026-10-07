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
- **M1 Foundation.** Workspace builds (`cargo test`, `cargo clippy -D warnings`, `cargo fmt`). `helix-desktop-api` reads `audio.sound_volume` from the real `helix-service` on a private bus and the bar shows it, updating when the setting changes (50% to 25% in the nested run). First frame 387-412 ms. Shell PSS with the panel hidden: 342 MB.
- **M2 Surface manager.** One Rust module owns every layer surface. Setup is hidden, size request, then `init_layer_shell`, then anchors, exclusive zone and keyboard mode. Escape and outside-click each close the open panel, and only one panel is open per monitor. Unit-tested with no display. Nested screenshots show the panel opening and closing. Shell PSS with the panel open: 409 MB, which is over the 350 MB budget. Stop. Do not start M3 until that cost comes down.
- **M3 Bar.** Real widgets driven by the existing config and the desktop service (clock, workspaces via niri IPC,
  volume, network, battery, tray later). Visual parity with the native bar on the same wallpaper.
- **M4 Assistant panel parity.** The session rules and their tests already live in `helix-shell-core` (including 700 ms arming, Enter never confirming, and Escape denying). This milestone is the panel UI at parity with the native one: Thinking, Do it/Cancel, stale answers dropped, display-safe text, focus back to the input.
- **M5 Launcher, control center, notifications, OSD.** One at a time, each behind a config flag so the C++ surface remains
  the default until parity is signed off.
- **M6 Decide lock screen and greeter.** They are trusted surfaces (ext-session-lock, greetd). Decide with measurements and
  a threat review before moving them; they may stay native.

## Budgets (record every milestone; do not let them drift)

| Metric | Native C++ shell today | Tauri spike | M1 bar | M2 bar + panel | Budget |
|---|---|---|---|---|---|
| Whole shell PSS | 105 MB | 227-336 MB (bar + panel only) | 342 MB | 409 MB | <= 350 MB with bar, one panel, notifications, OSD |
| Idle CPU (bar only) | not measured | ~0.07% | 0.20% | — | <= 0.3% |
| First frame | not measured | ~400 ms | 387-412 ms | panel shown at 22 s (first open, not startup) | <= 800 ms |
| Frontend bundle | n/a | ~10 KB | 2.2 KB gzipped | same bundle | <= 150 KB gzipped |

M1 and M2 were measured once in `tools/nested/run.sh` with the release binary (`rust/target/release/helix-shell-tauri`) and `tools/nested/process_tree.py`. PSS is the shell and its WebKit processes, not `helix-service` (16 MB in that run; 358 MB and 425 MB if it is included). Idle CPU is a 10 s sample of those shell processes after the bar had painted (`tools/nested/tauri-idle.sh`), 0.20%. A sample that included `helix-service` while it registered a Bluetooth agent was about 1.1% and is not the idle figure.

The panel-open PSS, 409 MB, is over the 350 MB budget. M2 stops here. Do not add notifications or OSD on this shell until the panel cost is understood. Pixel coverage in that run: bar strip 83.5%, open panel 44.6%, closed panel 0.1%. The bar text changed from `Sound 50%` to `Sound 25%` after `Configuration1.Update` of `audio.sound_volume` on the private bus. Screenshots: `/tmp/helix-nested/tauri-m2/panel-open.png` and `panel-closed.png`.

Gotcha from that run: do not call the bus name until the nested `helix-service` has claimed it. A client call first activates the installed `org.helix.Desktop` service, which inherits the dbus daemon's environment and opens the real session's config. `tools/nested/tauri-shell.sh` starts the service and execs the shell without a probe call. The shell retries `GetSettings` until the name is owned.

The live bar value is `audio.sound_volume` from `Configuration1.GetSettings`, not the PipeWire sink. Changing the sink would touch the person's real audio. The setting is portable (it does not require a Helix desktop) and `Changed` fires when it is updated.

Measure with `tools/nested/run.sh` and `tools/nested/process_tree.py` (sums PSS across WebKit helper processes).
If a milestone blows a budget, stop and report; do not push on.

## How to work

- Everything runs in the nested session, never the real one. Never `pkill -f`; stop processes by PID. Do not install files
  into `/usr/local` or restart the user's shell; installation is a separate, explicit step.
- Packages already installed for this: `webkit2gtk4.1-devel gtk3-devel gtk-layer-shell-devel libsoup3-devel javascriptcoregtk4.1-devel`.
- Commit small, one topic each, with the repo's message style. Update this document when a fact changes.
