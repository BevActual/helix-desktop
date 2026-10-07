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
- **M2 Surface manager (superseded by the single surface, below).** One Rust module owns every layer surface. Setup is hidden, size request, then `init_layer_shell`, then anchors, exclusive zone and keyboard mode. Escape and outside-click each close the open panel, and only one panel is open per monitor. Unit-tested with no display. Nested screenshots show the panel opening and closing. First measurement with both webviews kept alive: panel-open 409 MB (over budget). Stopped before M3.
- **M2 memory.** Creating the panel webview only while it is open, and `WebviewWindow::destroy` on dismiss, drops bar-only to 191 MB and panel-open to 255 MB (under the 350 MB budget, with ~95 MB headroom before notifications/OSD). `close()` instead of `destroy()` did not reclaim and ballooned back toward 409 MB; keep `destroy()`. Destroy alone does not exit the panel's `WebKitWebProcess` on Linux (reopen used to accumulate a third helper and ~300 MB idle). The shell snapshots bar-only WebKit PIDs and SIGTERMs any extra web-process descendants ~750 ms after panel destroy (`webkit_mem.rs`). Nested reclaim: after dismiss, webkit-web-count returns to 1 and shell PSS to ~201 MB within 1 s; a second open/close does not accumulate. `webkit_web_context_set_web_process_count_limit(1)` is a no-op on WebKit 2.54 (deprecated since 2.26; getter stays 0). M3 is unblocked on the peak budget.
- **M3 Bar.** Real widgets driven by the existing config and the desktop service (clock, workspaces via niri IPC,
  volume, network, battery, tray later). Visual parity with the native bar on the same wallpaper.
- **M4 Assistant panel parity (done, see below).** The session rules and their tests already live in `helix-shell-core` (including 700 ms arming, Enter never confirming, and Escape denying). This milestone is the panel UI at parity with the native one: Thinking, Do it/Cancel, stale answers dropped, display-safe text, focus back to the input.
- **M5 Launcher (done, see below), control center (sound and Wi-Fi done, see below), notifications, OSD.** One at a time, each behind a config flag so the C++ surface remains
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


## Decision: one surface, adopted (M2b, done)

The shell is now one layer surface hosting the bar and every panel (`ShellSurface` in `helix-shell-core`, applied by
`helix-shell-tauri/src-tauri/src/layer.rs`; frontend `dist/index.html` + `dist/shell.js`). The multi-surface manager
(`surfaces.rs`) is kept for surfaces that must be separate, such as the approval dialog.

Measured on the real crate (3 runs of `tools/nested/tauri-m2.sh`, nested niri, NVIDIA, shell excluding `helix-service`):

| State | Before (two surfaces) | Now (one surface) | Budget |
|---|---|---|---|
| Bar only | 296-297 MB | 323 MB | <= 350 MB |
| Panel open | 409 MB | 314-322 MB | <= 350 MB |
| After the panel closes | 305 MB | 326-330 MB | <= 350 MB |
| Idle CPU, shell + web processes | n/a | 0.15% | <= 0.3% |
| First frame | 370 ms | 311-342 ms | <= 800 ms |

Memory is now flat across states instead of spiking when a panel opens, at the price of about 26 MB more when idle.

Verified with `tools/nested/single-surface-pointer-test.sh` (a real virtual pointer and keyboard): click-through while closed,
typing confined to the panel while open, clicks inside keep it open, a click outside closes it without reaching the window
below, keyboard focus returns afterwards, Escape closes. The test found two bugs in this adoption that unit tests could not:
the Tauri capability still named the old windows (so the page received no events and the panel never appeared), and Escape
closed the surface state without applying the plan. `ShellSurface::escape` now only reports what to do.

Idle CPU of the C++ pieces, measured on the real session: `helix-service` 2.8% of a core (a permanent 16 ms poll timer), the
C++ shell 1.1%, the Tauri shell 0.1%. `helix-service` now ticks at 16 ms only while something is happening and at 100 ms otherwise
(0.85% to 0.25% in the harness). A fully event-driven loop (watching the PipeWire, mixer and HTTP fds) would remove the rest.

Follow-up on the real session (profiling with gdb samples, `dbus-monitor` for who calls what): the first fix alone did not help
because read-only calls counted as activity (about 11 calls a second reach the service: the Settings app calls `GetSettings` every
3 s and both it and the shell call `GetSnapshot` twice a second). Only calls that change something wake the fast tick now. The
remaining cost was `GetSettings` rebuilding a catalog of several hundred entries (including a filesystem scan for palettes) on every
call; it is now cached as a ready D-Bus variant and invalidated by every change notification except Network, Bluetooth and Audio
(which fire constantly on a real machine and are not in the catalog), by the desktop/gamma state, and after 30 s. Mutation-checked:
without invalidation `desktop_service` fails. Real-session idle CPU of `helix-service`: 2.9% before, 1.4-1.6% now. What remains is
mostly the polling itself; clients should listen to `Changed` signals instead of polling `GetSnapshot` and `GetSettings`.

## M4 verified: the assistant inside the Tauri shell, end to end

`tools/nested/assistant-flow-test.sh` (real 7B model, private bus, real `helix-service`, `helix-inference` and `helix-assistant`):
the panel opens, "Make the screen dimmer." is typed and submitted, the shell shows Thinking and then "About to: Make the screen dimmer
(display brightness, not sound) (`brightness-down`)" with Cancel and Do it. Two Enter presses start nothing. A real pointer click on
Do it starts the action, the result is shown, and focus returns to the input. The audit log shows `brightness-down` allowed under
the `helix-assistant` identity, not the shell's: the shell never held the model's authority. Run with
`HELIX_NESTED_ASSISTANT=<build>/helix-assistant` (pre-grants it, as an earlier approval would), `HELIX_MODEL_DIR` and `PATH` set on the
outer command (the model service is started by D-Bus activation and inherits the outer environment).

Known rough edges: the confirmation text includes the model-facing disambiguation ("display brightness, not sound"); the result text
is whatever the command printed ("ok"); the panel is a fixed 520 px.

## Polling removed from the C++ clients

The shell polled the desktop service on four timers (audio 1 s, Bluetooth 2 s, network 3 s, re-registration 3 s), each making a
synchronous `NameHasOwner` call first, and the Settings app refreshed seven calls every 3 s. All of it duplicated the `Changed`
signals the service already sends. Now: `DesktopClient::available()` trusts a bus name watch while the service is up (and still asks
the bus when it is absent, so a caller can never be stuck "unavailable"), audio and Bluetooth clients listen for `Changed` (audio
debounced 40 ms), the shell re-registers when the service appears, and every timer is a 30 s safety net. Measured idle calls to the
service: about 11 a second before; 0.7 a second in a nested session with the shell and Settings running
(`tools/nested/call-rate.sh`); on the real session the shell alone sends about 0.3 a second and its idle CPU fell from 1.1% to 0.8%.
Tauri clients should follow the same rule: subscribe to `Changed`, never poll.

## M5 launcher (done)

The launcher is a second panel on the single surface (`ShellSurface::PANELS = [assistant, launcher]`). Logic is in
`helix-shell-core/src/launcher.rs` (pure, mutation-checked): desktop entry parsing (only real applications; `NoDisplay`, `Hidden`,
`OnlyShowIn`/`NotShowIn` respected; translations ignored; names made display-safe), an `Exec` tokenizer that follows the Desktop Entry
spec and never involves a shell (field codes dropped, quotes and escapes handled, `; && $( )` are just text), and ranking (exact,
prefix, word start, substring, keywords/generic name, letters in order; shorter names and then alphabetical break ties).
`src-tauri/src/apps.rs` scans the data directories (earlier directories win; terminal applications are left out) every time the
panel opens and starts an application in its own process group with no stdio, then reaps it.

The page never holds a command: it asks `launcher_query(text)` for display-safe names and ids, and `launcher_launch(id)` can only name
an id from the current scan; unknown ids are refused and logged. Keyboard: type, Up/Down, Enter, Escape; a click on a row launches it.

Verified end to end by `tools/nested/launcher-test.sh` (fake applications that leave marker files, real keyboard and pointer): Enter
launches and closes the panel, hidden and terminal applications are never offered, no match plus Enter does nothing, Escape closes, a
click launches the first row, Down moves the selection, and a guard fails the test if anything but the fake applications launches.
(An earlier draft of that test launched a real application, Blender, because an empty query selects whatever sorts third on the
machine; tests must never use a query that can match real software.) Memory with the launcher: 334 MB bar only, 325 MB open, 337 MB
closed, flat over repeated cycles; still inside the 350 MB budget by about 15 MB, so the next panels need real attention to size.

## Experiment: can software rendering save memory? (no)

`tools/nested/mem-variant.sh` compares WebKit environment variants on the working single-surface shell (shell only, bar / panel open,
and whether anything paints, via `coverage.py`):

| Variant | Bar only | Panel open | Paints? |
|---|---|---|---|
| default (GPU path) | 334 MB | 324 MB | yes (83.5% / 49.5%) |
| `WEBKIT_DISABLE_DMABUF_RENDERER=1` | 243 MB | 248 MB | no (0.0% / 0.2%) |
| `WEBKIT_DISABLE_COMPOSITING_MODE=1` | 316 MB | 318 MB | no |
| both, or plus `LIBGL_ALWAYS_SOFTWARE=1` | 243-244 MB | 247-248 MB | no |

Without the DMA-BUF renderer the page runs (it reports ready, and a text caret region repaints) but the compositor only ever shows
damaged rectangles: the first full paint never arrives. Forcing a repaint after each resize (`queue_draw`), and nudging the page
from JavaScript at 0.4, 1, 2 and 4 s, did not help. About 90 MB is on the table if someone finds why the surface is not
repainted in software mode (possibly a GTK3/gtk-layer-shell shm-buffer interaction); until then the GPU path is the only one that paints,
and the earlier "do not set these variables" rule stands, now on firmer evidence.

### Launcher: icons and what you use

Icons: `apps.rs` resolves an entry's `Icon` (an absolute path, or a theme name looked up in each data directory's
`icons/hicolor/<size>/apps` and `pixmaps`) and returns PNG or SVG files up to 96 KB as `data:` URIs, cached per scan. A theme name is a
single path component; anything with `/` or `..` is refused. The page shows them in an `<img>`, which cannot run script, and the CSP
gained `img-src 'self' data:`. Only the HICOLOR theme is searched, so apps whose icons live in other themes show no icon.

History: `History` in `helix-shell-core` records launches by sequence number (no clock). With no query, recently launched apps lead
(most recent first), then alphabetical; with a query, use adds at most 60 points (frequency up to 30, plus 30 if among the last five
launches), which orders near-equal matches but never lifts a weaker kind of match over a stronger one (the gaps are 200). It keeps
200 entries. It is saved as `$XDG_STATE_HOME/helix/launcher-history.json` (directory 0700, file 0600, written by rename) and a damaged
file is an empty history. The nested test checks the modes. Mutation-checked: removing the cap, recency, restoring the sequence after
loading, the bound and the alphabetical fallback each fail a test.

## M5 control center: sound (done)

A third panel (`control-center`, top right; "Controls" in the bar). Sound only for now: the current output's volume slider, Mute/Unmute,
and the list of outputs (click one to make it the default). Nothing else is in it yet: network, Bluetooth, brightness, battery and the
rest of the quick settings come next.

Layers, each tested on its own:
- `helix-desktop-api`: `to_json` / `args_from_json` convert the service's `a{sv}` values (arrays, dictionaries, numbers, strings,
  booleans; NaN and infinity become null; an id above u32 is never truncated into an id; nested arguments are refused),
  `Desktop::audio_snapshot`, `audio_action` and `watch_audio` (follows `Audio1.Changed`, no polling). Each zbus proxy must be in its
  own module because the macro names signal types after the signal.
- `helix-shell-core/src/audio.rs`: `Audio::from_snapshot` (unavailable or malformed is "no audio"; unavailable devices are not listed;
  names display-safe; volume clamped), `view()`, and the only producers of write requests: `set_volume` (0 to 100, finite, always the
  current output), `set_muted`, `set_default` (the id must be an output in the snapshot just read: not a microphone, a program
  stream, an unplugged device or a forged id). The page never names a device for volume or mute. Mutation-checked (it found that no
  test had the default output anywhere but first).
- `src-tauri`: async commands `audio_set_volume`, `audio_set_mute`, `audio_set_default`, `audio_view`; a watcher that re-reads the
  snapshot after `Changed` (coalesced over 40 ms); the page throttles a drag to one write per 50 ms and ignores incoming updates while
  the thumb is held.

End to end (`tools/nested/control-center-test.sh`) against a private PipeWire with virtual sinks and a microphone
(`tools/nested/private-audio.sh`, `private-pipewire.conf`): no session manager and no ALSA, its socket in the scratch directory, so real
audio can never be touched; it is stopped by pid at the end of the run. Volume and mute are read back with `pw-dump`, not from the
service. A click at about 70% on the slider set the speakers to 73% with the other sink unchanged; Mute and Unmute round-tripped; choosing
the other output moved the "current" marker and the slider to that device (the service handles `default` without a session manager).

Memory: 334-335 MB bar only, 341 MB with the control center open. That is inside the 350 MB budget, but only by about 9 MB: the next
panels must come out of existing memory, not add to it.

Not done: input (microphone) volume, per-program volumes, the other quick settings, keyboard control of the slider is the browser's
default (arrow keys on the focused range input) and untested.

## Memory pass

**Measure with USS, not PSS.** PSS counts a share of every library page the process maps, so it moves with whatever else is running:
identical code measured 15-20 MB higher in a later session. `tools/nested/process_tree.py` now prints the shell's USS (private memory,
what stopping it would free), which is stable. Shell only, JIT off: **bar only 260 MB USS (327 PSS); after panels have been opened and
closed 290-295 MB USS (345-350 PSS)**. New budget: **<= 320 MB USS in every state** (PSS stays informational, about 55-70 MB higher).
The older "350 MB PSS" budget above is superseded by this.

What was tried, in alternating back-to-back runs so drift cannot fool the comparison:

| Change | Effect | Kept? |
|---|---|---|
| JavaScriptCore JIT off (`JSC_useJIT=false`, set before WebKit starts; an explicit environment value overrides it) | 5-6 MB USS, 7-13 MB PSS, every run; also no executable JIT memory in a process that draws model text | yes |
| Surface only bar-height while closed, full output when a panel is open | bar only 304 MB PSS instead of 334 (-30) **until the first panel opens**; after one open/close it is back at about 337 and stays: WebKit keeps the buffers | no |
| Open surface only 720 px tall instead of the whole output | no change at all after cycles (339 PSS) | no |
| `MALLOC_ARENA_MAX`, trim thresholds | under 3 MB | no |
| JSC concurrent GC/JIT off, one GC marker | 1 MB beyond JIT off | no |
| `WEBKIT_DISABLE_DMABUF_RENDERER` | about 90 MB, but nothing paints (see above) | no |

Where the rest is: the app process is about 120 MB (GTK, WebKit's UI side, about 25 MB of GPU driver libraries); the web process about 150 MB
(its anonymous heap, WebKit itself, JavaScriptCore, more GPU libraries); the network process 20 MB. Most of it is fixed cost of having a
WebKit web view at all. The first panel opened costs about 30 MB that never comes back.

## M5 control center: Wi-Fi (done)

The second section of the control center panel. Status ("Connected to X", "Connecting…", "Wi-Fi is off", "Network is unavailable"), a Wi-Fi
on/off button, Scan, Disconnect (only while connected), the visible networks, and VPN rows.

`helix-shell-core/src/network.rs` is the model and the only builder of requests, like audio:
- One row per network name (the same network is heard from several access points): the joined access point if any, otherwise the
  strongest; saved if any copy is saved. Order: joined, saved, strongest, name. At most 12 rows and 8 VPNs. Names are made display-safe for
  showing but the service is always sent the exact original name (a forget request carries it); the page names a network only by the path
  it was listed with.
- A click joins a network that needs no new secret (open, or already saved). It **refuses**, and sends nothing for: a secured network never
  joined ("Needs a password"), an enterprise network ("Enterprise"), the network already joined, an unknown path, Wi-Fi being off, no
  service. Passwords and enterprise sign-in are not built yet; the rows say so.
- Forget works only on a saved network; VPN toggles only on a listed path.
- Mutation-checked: each refusal, the exact-name rule, the ordering, the bounds, the path check and the de-duplication fail a test when
  removed (this found that the enterprise state in the row list had no test).

`helix-desktop-api` now has one generic path (`Desktop::snapshot`, `action`, `watch` over `Domain::{Audio, Network, Bluetooth}`, checked against
the contract) instead of a macro proxy per interface; audio goes through it and its end-to-end test is unchanged.

**Testing without a network.** `tools/nested/fake-desktop.py` is a scripted `org.helix.Desktop` (tools/nested/tauri-fake-desktop.sh runs the
shell against it): a joined saved network, an open one, a secured one never joined, an enterprise one and a VPN. It changes its world when
actions arrive, emits `Changed`, refuses what the real service would refuse, and logs every action. `tools/nested/network-test.sh` clicks every
control with a real pointer and asserts the exact action list: connect Cafe, disconnect, connect Home, forget "Home WiFi", vpn-connect, Wi-Fi
off, on, scan, and **nothing** for the password-protected and enterprise rows; the service refused nothing. The fake replaces the real service
for these tests, so the real service's NetworkManager bridge is not exercised by them.

**Safety rail.** The nested session used to be able to reach the real system bus: earlier runs saw real NetworkManager and BlueZ change signals.
Nothing wrote to them, but a Wi-Fi test could have. `tools/nested/run.sh` now points `DBUS_SYSTEM_BUS_ADDRESS` nowhere (a test may supply its own
with `HELIX_NESTED_SYSTEM_BUS`), so nothing started in a nested run can reach the person's real network, Bluetooth, or logind.

Memory with the full panel (sound plus Wi-Fi): 275 MB USS bar only, 294 MB open (including the fake service's Python process, so a little less
in practice); the budget is 320 MB USS.

Not done: entering a password for a secured network, enterprise sign-in, hidden networks, wired and cellular rows, Bluetooth, and a
password prompt flow (`Network1.Prompt` and `Reply`).
