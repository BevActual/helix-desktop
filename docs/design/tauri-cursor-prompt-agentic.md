# Prompt for Cursor: build toward the agentic shell

Paste everything below the line into Cursor in the helix-shell repo. (Supersedes the earlier `tauri-cursor-prompt.md`, whose milestones are done.)

---

You are continuing work on Helix, an agentic Linux desktop: niri compositor, a C++ desktop service `helix-service` (D-Bus
`org.helix.Desktop`, including the `Agent1` safety layer), a sandboxed model service `helix-inference`, an assistant client
`helix-assistant`, and a new Rust/Tauri shell that is replacing the C++ shell surface by surface. The goal is a desktop where an AI
assistant can act on the person's behalf **safely and visibly**: every action is tiered, granted, shown, confirmable and audited, and no
untrusted text can ever pass for a trusted decision.

## Where things stand (all verified; read before changing anything)

Read these first: `docs/design/tauri-rust-shell.md` (decisions, measurements, gotchas, every experiment that failed),
`docs/design/agent-and-inference.md` (Agent1 tiers, grants, confirmation, audit, the assistant), `rust/helix-shell-tauri/src-tauri/src/main.rs`,
and one finished feature end to end: audio (`rust/helix-shell-core/src/audio.rs` -> the commands in `main.rs` -> `dist/shell.js` ->
`tools/nested/control-center-test.sh`).

Built in `rust/`:
- `helix-shell-core`: pure logic, no GTK or D-Bus. `shell_surface` (the one surface for bar and panels), `assistant` (request session),
  `launcher`, `audio`, `network`, `bluetooth`, `quick` (dark theme, night light), `power` (battery), `confirm`, `display` (text made safe to show).
- `helix-desktop-api`: zbus client for `org.helix.Desktop`: generic `snapshot`/`action`/`watch` over `Domain`, `settings`/`update_setting`.
- `helix-shell-tauri`: one layer-shell surface hosting the bar and every panel in one web view (one WebKit process), a second empty
  surface that reserves the bar's space, panels: Assistant, Launcher, Control Center (Android-style tiles: Internet, Bluetooth, Dark theme,
  Night Light; volume slider; Internet/Bluetooth/Sound detail pages; battery). `org.helix.Shell1` on the session bus opens panels for key bindings.
It runs live beside the native shell as the user unit `helix-shell-tauri-trial` (the person is using it). C++ side: 141 tests pass.

## What to build next, in this order

1. **The approval dialog in the Tauri shell (the most important piece).** Today the native C++ shell is the registered Agent1 *confirmer*
   (`RegisterConfirmer`, `ConfirmationRequested`/`ConfirmationEnded` signals, `Confirm`; see `src/shell/agent/`). The Tauri shell must be able to
   take over that role. It must be a **separate, trusted surface**, not a panel inside the main page: an Overlay-layer surface with exclusive
   keyboard, created on demand and destroyed after, rendering only text that passed `sanitize_for_display`, naming the verified client, the
   tool and its arguments, the model's stated reason, a taint warning, and (for first-use consent) tier checkboxes where `confirm` is opt-in.
   Port every rule of `AgentConfirmPanel`: **Enter never approves**, Allow is disabled for 700 ms after each request, Escape and closing deny,
   the service's `ConfirmationEnded` closes a stale dialog, requests queue. Put the logic in `helix-shell-core` as a state machine with mutation-checked
   tests; test it nested against the real `helix-service` (see `tools/nested/assistant-flow-test.sh`) and with a real pointer.
   The shell's boundary test currently forbids touching Agent1: carve out exactly `RegisterConfirmer`, `Confirm` and the two signals, and keep
   everything else (requesting access, running actions, the model) forbidden. Document the carve-out in the plan.
2. **An agent activity view.** What the assistant did, when, and with whose approval, from the Agent1 audit (manager-role calls are limited to the
   Settings app by design: do not widen that; propose a read-only audit stream the shell may subscribe to, in `protocols/dbus/`, and implement
   it in the C++ service with tests). Include a visible indicator in the bar while an agent action is running and a one-click "stop agent" that
   cancels the pending operation.
3. **The assistant, further.** Keyboard binding hints, request history, clearer results than the command's raw output ("ok"), and
   multi-step requests only with a visible plan the person approves as a whole. Never offer `confirm`-tier tools to the model without the dialog from item 1.
4. **Notifications.** An `org.freedesktop.Notifications` server in the Tauri shell (toasts, history, do-not-disturb, a shade next to the
   quick-settings tiles). All notification text is hostile input: it must never be able to look like an approval dialog or a system message.
5. **What the native shell still does that the bar needs.** Workspaces and window state via the niri IPC socket, the system tray
   (StatusNotifierItem), brightness through logind (read-only probing first; never elevate), media controls (MPRIS), keep-awake. Then decide with a threat
   review, not by default, whether lock screen and greeter move; they are trusted surfaces and may stay native.
6. **Finish the control center.** Bluetooth pairing and Wi-Fi password prompts (the `Network1.Prompt`/`Reply` and `Bluetooth1.Prompt`/`Reply` flows;
   secrets must never be logged or kept), enterprise Wi-Fi.

Do not start item N+1 until item N meets its definition of done.

## How every feature is built here (follow the pattern; it is what made the work trustworthy)

1. A **pure model in `helix-shell-core`** that turns the service's snapshot into a view and is the **only producer of write requests**
   (validation lives there: ranges, ids that must come from the last snapshot, what is refused and why). No D-Bus, no GTK.
2. Contracts in `protocols/dbus/*.xml` are the source of truth; `helix-desktop-api` checks the methods it calls against them. New interfaces get an XML file and a test.
3. Thin Tauri commands that build a request from the model and send it; a watcher that follows `Changed` signals. **Never poll the service.**
4. The page draws what Rust sends. Text is assigned with `textContent` only. No `innerHTML`, no `eval`, strict CSP, minimal capabilities (the capability
   file must name the window label or events silently stop).
5. **Mutation-check every test**: break the code on purpose and show the test fails; fix test gaps the way the existing commits did.
6. An **end-to-end test in the nested session** that drives the real UI with a real pointer and keyboard and asserts the exact actions the
   service received, including what must **not** be sent. Controls are found by name (`data-test`, `tools/nested/ui-lib.sh` `click_id`), never by pixel.
   Devices are never real: `tools/nested/fake-desktop.py` (Wi-Fi, Bluetooth, settings), `tools/nested/private-audio.sh` (a private PipeWire read back with `pw-dump`).

## Hard rules (breaking one means the task failed)

- **Never touch the person's real session.** Test only with `tools/nested/run.sh`. The nested session is cut off from the real system bus on purpose
  (no real NetworkManager, BlueZ or logind); keep it that way. Do not restart their units, install into `/usr/local`, or run anything with sudo.
  Deploying to the live trial is the person's call: say what to run, do not run it.
- **Never delete or kill by pattern.** No `rm` with a glob under `/run/user/1000` or any path the real session uses (a glob once deleted the real niri
  socket and forced a logout); no `pkill -f`. Delete exact paths your own harness created and stop processes by PID.
- **Never screenshot the real screen.** Screenshots are for the nested compositor only.
- **The shell never holds the model's authority.** The assistant stays a separate process (`helix-assistant`, its own Agent1 identity and grants). The only Agent1
  methods the shell may call are the confirmer ones from item 1.
- **Trusted text is never confusable with untrusted text.** Anything from a model, app, notification, window title, file name or the desktop is sanitised and
  can never appear inside a surface that asks for approval except as clearly quoted data.
- Do not set `WEBKIT_DISABLE_*` or software GL (nothing paints); do not add a second web process for panels; keep the budget below.
- **Memory budget: at most 320 MB USS** for the whole shell in every state (measure with `tools/nested/process_tree.py`; PSS drifts with other
  processes, USS does not). It is about 250 MB now. Anything that adds memory must say how much and what it displaced. Idle CPU of the shell at most 0.3%.
- No new polling anywhere; a new timer needs a stated reason.

## Gotchas already paid for (do not rediscover them)

- Layer surfaces need `set_size_request` before `init_layer_shell`; a four-edge surface ignores its exclusive zone, so a second empty surface reserves the
  bar's space and the shell surface uses zone -1. Open panels need an input region over the whole surface (a scrim catches outside clicks); closed, only the bar.
- Each zbus proxy macro must live in its own module (the generated signal types clash). `std::env::set_var("JSC_useJIT","false")` runs before WebKit starts.
- sysfs files report a size of 4096 bytes whatever they hold: never trust file sizes for `/sys`.
- A test that picks a query matching real software will launch it: tests use only fake applications.
- Click positions come from the reported layout; if a control is not in `layout.json`, the page did not draw it.

## Definition of done for each item

`cargo test` and `cargo clippy -D warnings` pass in `rust/`; `meson test -C build-debug` still passes (141 tests); every new rule has a mutation-checked
test; the nested end-to-end test passes and its exact-action assertions cover what must not happen; `docs/design/tauri-rust-shell.md` records the design,
the measurements (USS, idle CPU) and anything that failed; small commits, one topic each, in the repo's message style; and a short report of what you
changed, what you measured, and what you are unsure about. If a budget is exceeded or a rule would have to bend, stop and report instead of continuing.
