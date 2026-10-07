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
- **M1 Foundation.** Workspace builds (`cargo test`, `cargo clippy -D warnings`, `cargo fmt`). `helix-desktop-api` reads `audio.sound_volume` from the real `helix-service` on a private bus and the bar shows it, updating when the setting changes (50% to 25% in the nested run). First frame 387-412 ms. Shell PSS with the panel hidden was 342 MB when both webviews were created at startup.
- **M2 Surface manager.** One Rust module owns every layer surface. Setup is hidden, size request, then `init_layer_shell`, then anchors, exclusive zone and keyboard mode. Escape and outside-click each close the open panel, and only one panel is open per monitor. Unit-tested with no display. Nested screenshots show the panel opening and closing. First measurement with both webviews kept alive: panel-open 409 MB (over budget). Stopped before M3.
- **M2 memory.** Creating the panel webview only while it is open, and `WebviewWindow::destroy` on dismiss, drops bar-only to 191 MB and panel-open to 255 MB (under the 350 MB budget, with ~95 MB headroom before notifications/OSD). `close()` instead of `destroy()` did not reclaim and ballooned back toward 409 MB; keep `destroy()`. Destroy alone does not exit the panel's `WebKitWebProcess` on Linux (reopen used to accumulate a third helper and ~300 MB idle). The shell snapshots bar-only WebKit PIDs and SIGTERMs any extra web-process descendants ~750 ms after panel destroy (`webkit_mem.rs`). Nested reclaim: after dismiss, webkit-web-count returns to 1 and shell PSS to ~201 MB within 1 s; a second open/close does not accumulate. `webkit_web_context_set_web_process_count_limit(1)` is a no-op on WebKit 2.54 (deprecated since 2.26; getter stays 0). M3 is unblocked on the peak budget.
- **M3 Bar.** Real widgets driven by the existing config and the desktop service (clock, workspaces via niri IPC,
  volume, network, battery, tray later). Visual parity with the native bar on the same wallpaper.
- **M4 Assistant panel parity.** The session rules and their tests already live in `helix-shell-core` (including 700 ms arming, Enter never confirming, and Escape denying). This milestone is the panel UI at parity with the native one: Thinking, Do it/Cancel, stale answers dropped, display-safe text, focus back to the input.
- **M5 Launcher, control center, notifications, OSD.** One at a time, each behind a config flag so the C++ surface remains
  the default until parity is signed off.
- **M6 Decide lock screen and greeter.** They are trusted surfaces (ext-session-lock, greetd). Decide with measurements and
  a threat review before moving them; they may stay native.

## Budgets (record every milestone; do not let them drift)

| Metric | Native C++ shell today | Tauri spike | M1/M2 (both webviews alive) | M2 memory (destroy-on-close) | Budget |
|---|---|---|---|---|---|
| Whole shell PSS (bar only) | 105 MB | 227-336 MB (bar + panel) | 342 MB | 191 MB | <= 350 MB with bar, one panel, notifications, OSD |
| Whole shell PSS (panel open) | — | — | 409 MB | 251-255 MB | same |
| After panel destroy (1 s, with reap) | — | — | — | ~201 MB (1 WebKit web process) | — |
| Idle CPU (bar only) | not measured | ~0.07% | 0.20% | not remeasured | <= 0.3% |
| First frame | not measured | ~400 ms | 387-412 ms | bar 308-351 ms | <= 800 ms |
| Frontend bundle | n/a | ~10 KB | 2.2 KB gzipped | same | <= 150 KB gzipped |

**Independent verification (3 runs of `tools/nested/tauri-m2.sh`, current tree, release build).** The M2 figures above did not
reproduce. Measured, shell only (excluding `helix-service`): bar only 296-297 MB (one web process 154 MB plus the app process
about 122 MB plus network process 21 MB); panel open 409 MB (two web processes, 132 + 131 MB); after the panel is destroyed 305 MB
(the reap lowers the process count but memory stays high). Idle CPU 0.67-0.57 cpu-seconds over 8 s (about 0.1%), first frame 370 ms,
pixel coverage as reported (bar 83.5%, panel open 44.6%, closed 0.1%). So the bar alone is within the 350 MB budget, bar plus an
open panel is not, and destroy-on-close does not return to the bar-only level. The app process is about 120 MB in this harness for the original spike too (116 MB, measured the same way), so the shell code added
nothing; an earlier "57 MB" figure came from different measurement conditions and should be ignored. Most of it is WebKit and the
GPU userspace stack (about 41 MB heap, 25 MB anonymous, about 25 MB of NVIDIA, Mesa and LLVM libraries mapped in each process),
not allocator behaviour: `MALLOC_ARENA_MAX`/trim settings changed it by under 3 MB. Same harness, original spike (bar plus hidden panel,
two web processes): 342 MB total, so the current tree already saves about 45 MB at idle by keeping to one web process.

Measured in `tools/nested/run.sh` with the release binary and `tools/nested/process_tree.py` (prints total PSS and shell-only excluding `helix-service`). Use `tools/nested/tauri-reclaim.sh` to watch WebKit PIDs across destroy. The 409 MB figure was two WebKit renderers created at startup (hidden panel still ~72 MB). Lazy panel create + destroy on dismiss fixes the peak; PID reap fixes the post-dismiss leak. Pixel coverage unchanged: bar strip 83.5%, open panel 44.6%, closed 0.1%. Screenshots: `/tmp/helix-nested/tauri-m2/panel-open.png` and `panel-closed.png`.

Gotcha from that run: do not call the bus name until the nested `helix-service` has claimed it. A client call first activates the installed `org.helix.Desktop` service, which inherits the dbus daemon's environment and opens the real session's config. `tools/nested/tauri-shell.sh` starts the service and execs the shell without a probe call. The shell retries `GetSettings` until the name is owned.

The live bar value is `audio.sound_volume` from `Configuration1.GetSettings`, not the PipeWire sink. Changing the sink would touch the person's real audio. The setting is portable (it does not require a Helix desktop) and `Changed` fires when it is updated.

Measure with `tools/nested/run.sh` and `tools/nested/process_tree.py` (sums PSS across WebKit helper processes).
If a milestone blows a budget, stop and report; do not push on.

## How to work

- Everything runs in the nested session, never the real one. Never `pkill -f`; stop processes by PID. Do not install files
  into `/usr/local` or restart the user's shell; installation is a separate, explicit step.
- Packages already installed for this: `webkit2gtk4.1-devel gtk3-devel gtk-layer-shell-devel libsoup3-devel javascriptcoregtk4.1-devel`.
- Commit small, one topic each, with the repo's message style. Update this document when a fact changes.


## Experiment: one surface for the bar and all panels (`rust/experiments/single-surface`)

The cost that breaks the budget is the second web process that appears when a panel opens (about 131 MB). A single full-output
layer surface avoids it: the bar and the panel live in one page, one web process, with the input region trimmed so clicks
outside the visible parts pass through.

Measured (same harness): bar only 323 MB (app 144 + web 158); panel open 317 MB, still one web process. Compare the
current two-surface tree: 297 MB bar only, 409 MB with a panel open. Pixel coverage with the panel open: bar 83.5%, panel 32.0%
(a smaller panel than before). It fits the 350 MB budget in both states; the two-surface design does not when a panel is open.

Verified with a real pointer and keyboard (`tools/nested/single-surface-pointer-test.sh`, using `tools/nested/wl-pointer`, a virtual-pointer
injector, and `tools/nested/probe_window.py`, a window that logs the events it receives). All nine checks pass:
- Panel closed: clicks on empty areas reach the window below (click-through); clicks on the bar are handled by the bar.
- Panel open: typed text goes to the panel and never to the window below; clicking inside it keeps it open; clicking outside
  closes it and that click does not reach the window below; Escape closes it.
- After closing, keyboard focus returns to the window below and click-through works again.
- Bug found and fixed by the test: the input region must be re-applied on every resize (`connect_size_allocate`), not once on a
  timer, or the first state lets empty areas swallow clicks.

Still open:
- Bar-only is about 26 MB higher (a full-output transparent buffer).
- Raising the whole surface to Overlay while a panel is open puts the bar above fullscreen windows for that time.
- One page means one crash or hang takes the bar and the panels down together; per-output surfaces are needed for multi-monitor.
- Layer and keyboard-mode switching was not checked for visible flicker, only for correctness.
- The exclusive zone (space reserved for the bar) was not checked against tiled window placement.

Recommendation: adopt it for the shell UI (bar, launcher, control center, assistant); keep the approval dialog, lock screen and
greeter as separate trusted surfaces regardless.
