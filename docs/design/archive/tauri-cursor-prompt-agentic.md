# Prompt for Cursor: build toward the agentic OS

Paste everything below the line into Cursor in the helix-shell repo. (Supersedes the earlier `tauri-cursor-prompt.md`, whose milestones are done.)

---

You are continuing work on Helix, an **agentic Linux operating system**: a desktop where an AI assistant can act on the person's behalf
**safely and visibly**. Every action is tiered, granted, shown, confirmable and audited, and no untrusted text can ever pass for a trusted
decision. The shell is only one part of this. The parts:

- niri compositor (fork "26.04 helix").
- `helix-service`: C++ session service, D-Bus `org.helix.Desktop` (Configuration1, Appearance1, Audio1, Network1, Bluetooth1, Displays1,
  **Agent1** = the safety layer: roles client/confirmer/manager, tiers observe / observe-private / act / confirm / blocked from
  `protocols/ipc/msg-risk.toml`, per-client grants, taint tracking, append-only audit).
- `helix-inference`: sandboxed model service (llama.cpp, Vulkan, 7B Qwen2.5 Q4, grammar-constrained JSON tool calls, `org.helix.Inference`).
- `helix-assistant`: the assistant client, a separate process with its own Agent1 identity and grants.
- The Rust/Tauri shell in `core/` / `shell/` / `apps/` / `services/`, replacing the C++ shell surface by surface (the C++ shell stays until parity).

## Where things stand (all verified; read before changing anything)

Read first: `docs/design/agent-and-inference.md` (the agent and model design, measured accuracy), `docs/design/tauri-rust-shell.md`
(decisions, measurements, gotchas, every failed experiment), `protocols/dbus/*.xml` (the contracts), `protocols/ipc/` (risk policy, tool hints),
and one finished feature end to end: audio (`core/helix-shell-core/src/audio.rs` -> commands in `shell/helix-shell-tauri/src-tauri/src/main.rs`
-> `dist/shell.js` -> `tools/nested/control-center-test.sh`).

Working today: a person types a request, the model returns one grammar-constrained tool call, the shell shows the exact call, Agent1
checks tier and grant, risky actions go through a confirmation dialog (arming delay, Enter never approves), the action runs, the audit logs it.
Held-out exact accuracy ~90%; correctly declining impossible requests only 3/8 (so the UI always shows the call before running a change).
Shell: bar, assistant panel, launcher, Android-style control center (Internet, Bluetooth, Dark theme, Night Light, volume, battery),
`org.helix.Shell1` for key bindings, running live beside the native shell as user unit `helix-shell-tauri-trial`. 141 C++ tests and the Rust suites pass.

## Workstreams, in priority order (do not start N+1 before N meets its definition of done)

1. **Trusted approval surface in Rust.** The native C++ shell is the Agent1 *confirmer* today. The Tauri shell must be able to take that role as a
   **separate trusted surface**, not a panel in the main page: Overlay layer, exclusive keyboard, created on demand and destroyed after,
   text only through `sanitize_for_display`, showing the verified client, tool and arguments, the model's reason, a taint warning, first-use
   consent with tier checkboxes where `confirm` is opt-in. Port every rule of the C++ `AgentConfirmPanel`: Enter never approves, Allow disabled 700 ms
   after each request, Escape/close denies, `ConfirmationEnded` closes a stale dialog, requests queue. State machine in `helix-shell-core`, mutation-checked,
   tested nested against the real service (`tools/nested/assistant-flow-test.sh`) with a real pointer. The shell boundary test forbids Agent1 today:
   carve out exactly `RegisterConfirmer`, `Confirm` and the two signals, document it.
2. **Agent visibility and control.** A read-only audit/activity stream (new D-Bus interface in `protocols/dbus/`, implemented in the C++ service with tests;
   do not widen the manager role, which belongs to Settings by design), an activity view (what, when, whose approval, what it touched), a bar indicator
   while an agent action runs, a one-click **stop agent** that cancels the pending operation and revokes nothing silently, a grants view (per client, per tier,
   revoke), and a **kill switch / safe mode**: the whole desktop must work with the agent layer disabled or crashed.
3. **Reach: what the agent can safely do.** Grow the tool surface only through the existing mechanism (command schema + `msg-risk.toml` classification +
   `tool-hints.toml`; an unclassified command is blocked). Candidates: windows and workspaces via niri IPC, app launch/close, notifications, media,
   brightness, files and clipboard (these are `observe-private` or `act` with taint: design the taint rules first and write the threat model
   before code). Every new tool needs a risk tier with a stated reason, a hint line, grammar coverage, and cases in the held-out eval set.
4. **The model and its eval.** An eval harness that runs the held-out set (and new cases for every tool added) against the real `helix-inference` and reports exact
   accuracy and decline accuracy; a regression gate. Improve decline behaviour (more decline cases, prompt/grammar changes) and measure; consider multi-step
   requests only as a **visible plan the person approves as a whole**, each step still tiered. Model lifecycle: load on demand, VRAM budget, unload on idle,
   behaviour when the GPU is busy or the model is missing. Any model change reports accuracy, latency and VRAM before and after.
5. **Context and memory, with the person in control.** What the agent may know (current windows, selection, recent notifications) is `observe` or
   `observe-private`; anything it remembers is stored locally in a visible, editable, deletable store; nothing is sent off the machine. Untrusted content
   (web, files, notifications, other apps' text) taints the session and the existing taint rules apply. Write the design doc and threat model first and get it
   reviewed by the person before implementation.
6. **Third-party agents and tools.** Other clients get their own Agent1 identity and grants (the model exists). Design, as a doc first, how an app or
   outside agent offers tools to the OS (manifest, classification supplied by the app is never trusted: the OS assigns the tier) and how the person reviews them.
7. **Shell parity** so the Tauri shell can become the session shell: notifications server (toasts, history, do-not-disturb, shade; all notification text is hostile),
   bar workspaces and tray (niri IPC, StatusNotifierItem), brightness (logind, probe read-only), media (MPRIS), keep-awake, Wi-Fi password and Bluetooth pairing
   prompts (`Prompt`/`Reply`; secrets never logged or kept), Settings app port, multi-monitor. Lock screen and greeter are trusted surfaces: decide by threat review
   whether they move; they may stay native.
8. **OS integration.** Packaging the Rust shell into `tools/install-assistant.sh` (staged install only), systemd user units, a session entry, first-run consent
   and an explanation of what the agent can do, update and rollback, and making the Tauri shell the default only after parity and the person's go-ahead.

## How every feature is built here

1. A **pure model in `helix-shell-core`** (no D-Bus, no GTK) that turns a snapshot into a view and is the **only producer of write requests**; all validation
   lives there (ranges, ids that must come from the last snapshot, what is refused and why).
2. Contracts in `protocols/dbus/*.xml` are the source of truth; clients check the methods they call against them. New interface = new XML + a test.
3. Thin commands that send what the model produced; watchers follow `Changed` signals. **Never poll.** A new timer needs a stated reason.
4. Page text is set with `textContent` only. No `innerHTML`, no `eval`, strict CSP, minimal Tauri capabilities (the capability file must name the window
   label or events silently stop).
5. **Mutation-check every test**: break the code on purpose, show the test fails, fix gaps. Security rules get negative tests (what must NOT happen).
6. An **end-to-end test in the nested session** driving the real UI with a real pointer/keyboard and asserting the exact calls the service received.
   Controls are found by name (`data-test`, `tools/nested/ui-lib.sh` `click_id`), never by pixel. Devices are fakes: `tools/nested/fake-desktop.py`,
   `tools/nested/private-audio.sh` (private PipeWire, read back with `pw-dump`).

## Hard rules (breaking one means the task failed)

- **Never touch the person's real session.** Test only with `tools/nested/run.sh` (it is cut off from the real system bus on purpose). No sudo, no
  installing into `/usr/local`, no restarting their units. Deploying to the live trial is their call: say what to run, do not run it.
- **Never delete or kill by pattern.** No `rm` with a glob under `/run/user/1000` or any path the real session uses (this once deleted the real niri socket and
  forced a logout); no `pkill -f`. Delete exact paths your harness created; stop processes by PID.
- **Never screenshot the real screen**, nested compositor only.
- **The shell and desktop service never hold the model's authority.** The assistant stays a separate process. The model proposes; Agent1 disposes. Nothing the
  model says is ever a trusted decision.
- **Trusted text is never confusable with untrusted text.** Model, app, notification, window title and file text are sanitised and appear in approval
  surfaces only as clearly quoted data.
- Do not set `WEBKIT_DISABLE_*` or software GL (nothing paints); one web process for the shell.
- **Budgets:** shell at most 320 MB USS in every state (`tools/nested/process_tree.py`; PSS drifts, USS does not; ~250 MB now), idle CPU at most 0.3%, first frame
  at most 800 ms. Anything adding memory says how much. Inference VRAM is budgeted and reported separately.
- Never commit secrets; the person's sudo password was once pasted in chat: never use or repeat it.

## Gotchas already paid for

- Layer surfaces need `set_size_request` before `init_layer_shell`; a four-edge surface ignores its exclusive zone, so a second empty surface reserves bar space and
  the shell surface uses zone -1. Open panels need a whole-surface input region; closed, bar only.
- Each zbus proxy macro lives in its own module. `JSC_useJIT=false` is set before WebKit starts.
- sysfs files report size 4096 whatever they hold; never trust `/sys` file sizes.
- A test whose query matches real software will launch it; tests use fake applications only.
- If a control is missing from `layout.json`, the page did not draw it.

## Definition of done (every item)

`cargo test` and `cargo clippy -- -D warnings` pass in `core/` / `shell/` / `apps/` / `services/`; `meson test -C build-debug` passes; every new rule has a mutation-checked test including negative
cases; the nested end-to-end test passes and asserts exact calls; the relevant design doc records the design, measurements (accuracy, USS, CPU, VRAM) and what failed;
small single-topic commits in the repo's style; a short report of what changed, what was measured, and what you are unsure about. If a budget is exceeded or a
rule would have to bend, stop and report.
