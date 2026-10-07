# Prompt for Cursor: start the Tauri/Rust shell

Paste everything below the line into Cursor in the helix-shell repo.

---

You are continuing work on Helix, an agentic Linux desktop (niri compositor, a C++ shell with a custom renderer, a D-Bus
service `helix-service`, a sandboxed model service `helix-inference`, and an assistant client `helix-assistant`). The
owner has decided to replace the C++ shell and apps with Rust + Tauri, surface by surface, keeping the C++ shell running
until each replacement reaches parity.

Read these first, in this order, before writing any code:
1. `docs/design/tauri-rust-shell.md`: the decision, the measured evidence, the requirements and the milestones. It lists
   gotchas that already cost hours (layer-shell sizing, env vars that break WebKit). Do not rediscover them.
2. `docs/design/agent-and-inference.md`: the Agent1 security model and the assistant (including "The shell panel").
3. `rust/helix-shell-tauri/src-tauri/src/main.rs` and `dist/`: the working spike (a layer-shell bar and an assistant panel).
4. `src/shell/assistant/assistant_session.{h,cpp}` and `tests/assistant_session_test.cpp`: the state machine and the rules
   its tests enforce. The Rust port must keep every rule and every test case.

Your task is Milestone M1 and M2 from the plan:
- M1: make `rust/` a proper workspace (tests, `cargo clippy -D warnings`, formatting), add `helix-shell-core` (pure logic,
  no Tauri/GTK) and `helix-desktop-api` (a `zbus` client for `org.helix.Desktop`, generated from or checked against
  `protocols/dbus/org.helix.Desktop.xml`). Show one live value from `helix-service` updating in the bar.
- M2: write a single surface manager in Rust that owns every layer surface (create hidden, set_size_request, init layer
  shell, anchors, exclusive zone, keyboard mode, show/hide, Escape and outside-click dismissal, one panel at a time).
  Unit-test its state machine without a display.

Hard rules (violating any of these is a failed task):
- Contracts in `protocols/` are the source of truth; do not create parallel APIs.
- The assistant stays a separate process (`helix-assistant --json`). The shell binary must never hold Agent1 grants or talk
  to the model.
- All text from a model, app, notification, window title or the desktop is set as text, never as markup. Strict CSP, no
  remote content, no `eval`, minimal Tauri capabilities, typed validated command arguments.
- Enter never confirms an action; confirmation is armed after 700 ms; Escape denies. Port the behaviour and the tests.
- Tests must be mutation-checked: after writing a test, break the code on purpose and show the test fails.
- Test only in the nested session (`tools/nested/run.sh`, `tools/nested/coverage.py`, `tools/nested/process_tree.py`). Do not touch the real
  session, do not install into `/usr/local`, do not restart the user's shell, never run `pkill -f`.
- Record PSS, idle CPU, first-frame time and bundle size at the end of each milestone against the budgets in the plan. If a
  budget is exceeded, stop and report instead of continuing.
- Commit small and often, one topic per commit, matching the existing commit message style. Update
  `docs/design/tauri-rust-shell.md` whenever you learn a fact that changes it.

Definition of done for this task: `cargo test` and `cargo clippy -D warnings` pass in `rust/`; the existing `meson test` suite
still passes; a nested-session screenshot shows the bar with a live value and the surface manager opening and closing a
panel; the budget table is updated with measured numbers; and you give a short report of what you changed, what you
measured, and what you are unsure about.
