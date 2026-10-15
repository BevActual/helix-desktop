# Agent Surface v2 and built-in inference

Status: living design + implementation notes. Wire contracts for Agent Surface / Inference1 live in
`protocols/dbus/org.helix.Desktop.xml` (and related). `agentic-os.md` is the product north star;
sections below mark what ships versus what is still open.
Product homes: `services/helix-assistant` and `services/helix-inference` (Rust).
Speech worker is Cargo (`helix-transcribe`). Interim llama lives under
`services/helix-inference/native/`; `helix-image` is a Cargo crate over a C engine shim
under `services/helix-image/native/` (`cpp-to-rust.md`).

## Why a second design

v1 is `tools/helix-agent` (MCP) in front of `helix-panel`. Its risk policy
(`protocols/ipc/msg-risk.toml`) is enforced **in the adapter only**. The IPC socket accepts every command
from any process of the user, and the `confirm` tier has no confirmation path. v2 moves enforcement into a
service every agent client must go through, and adds the built-in model runtime those clients talk to.

## Threat model

| Actor | Can | Must not |
|---|---|---|
| Granted agent client (MCP adapter, Assistant, Agent Center) | call tools in its granted tiers | exceed its tiers, confirm its own actions, read the audit log |
| Content the agent reads (web page title, notification, clipboard, screenshot) | influence the model | cause a `confirm`-tier action without a human decision |
| Hostile process of the same user | open any user socket | be mistaken for a granted client |
| Model file | be malformed | execute code outside the inference sandbox |

**Limit we cannot design away:** a same-user process can always connect to the shell's IPC socket. D-Bus
identity and grants constrain *cooperative* clients (our own apps, MCP adapters). Constraining a hostile
same-user process needs sandboxing: Flatpak or `systemd-run --user` with a restricted filesystem view for
agent runners, and SELinux confinement on Fedora. The agent socket and D-Bus names are therefore the only
route we hand to sandboxed agent clients; the human `helix-panel` socket stays for the user's own tools.

## Agent Surface

Served by `helix-service` (D-Bus activated, runs without the shell). One interface, three roles:

- **Client.** `Hello` registers a label; identity is read from the connection (pid, `/proc/<pid>/exe`,
  Flatpak app id), not from arguments. `ListTools` returns the tools the caller may use, generated from
  `helix schema-json` plus `msg-risk.toml`. `Action(tool, args)` returns an operation id. `Completed`
  goes to the requester only, as in the other interfaces.
- **Confirmer.** The shell calls `RegisterConfirmer` (one owner, same pattern as `RegisterDesktop`). A
  `confirm`-tier action emits `ConfirmationRequested` to that owner only; the shell shows a native dialog
  naming the client, tool, arguments and the agent's stated `reason`. Only the confirmer may call
  `Confirm`. If the confirmer disconnects, every pending confirmation is denied. A client can never confirm
  its own request: the confirmer is a different bus connection by construction.
- **Manager.** Settings registers as manager to read and edit grants and read the audit log.

### First-use consent

A client with no grants calls `RequestAccess(tiers)`. The service sends the confirmer a request of kind
`grant` naming the client (label and verified identity) and the tiers asked for. The user can approve fewer
tiers than requested; `blocked` is never offered. The grant is written to
`<state dir>/agent/grants.json` (mode 0600; `~/.local/state/helix/` until the rename) and applied only if the write succeeds. Tiers a client
already holds complete immediately without a dialog. Revoking a client or editing its tiers later is the
manager role's job (Settings). A request with no confirmer present is denied, so a headless session cannot
grant itself access.

### Tiers and flow

`observe`, `observe-private`, `act`, `confirm`, `blocked` as in `msg-risk.toml`. Grants are per client and
per tier and persist in the service's state directory. `blocked` is never grantable.

1. Client calls `Action`.
2. Service resolves tier (command, then choice override), checks the client's grant, appends an audit
   record, then runs the command through the shell (internal hop to the shell socket).
3. `confirm`: service emits `ConfirmationRequested` and waits (deadline 60 s, then denied).
4. `Completed(op, ok, result)`. `result` is JSON with `trust: "untrusted"` for any `observe-private` output
   and for anything derived from clipboard, notifications, window titles or screenshots.

### Injection handling

Untrusted results are labelled by the service, not by the client, so a careless client cannot strip the
label. Rule enforced by the service: after a client receives an `untrusted` result, its next `confirm`-tier
request carries `tainted: true` in the confirmation, and the dialog says so. Taint clears after a human
confirms or the client reconnects. This does not stop injection; it makes the human decision informed.

### Audit

Append-only JSON lines in `<state dir>/agent/audit.jsonl`: time, client, tool, argv (literal
arguments truncated, secrets never logged), tier, decision, outcome. Readable by the manager role
(`GetAudit`) and by the shell via `AgentSurfaceActivity1.GetActivity` (below). Clients still must not read it.

### AgentSurfaceActivity1 (shell visibility; workstream 2)

New interface `org.helix.Desktop.AgentSurfaceActivity1` on the same object path. **Only the registered
desktop** (`RegisterDesktop` owner) may call it — not every session process, and not by widening
`RegisterManager`. Settings keeps the manager role for `SetGrant` and the full Agents page.

| Method | Purpose |
|---|---|
| `GetActivity(since, limit)` | Read-only audit rows (same JSON shape as manager `GetAudit`) |
| `ListPending` | Operations waiting for a confirmation (bar indicator). Operations already running are not listed or stoppable yet |
| `Stop(operation)` / `StopAll` | Cancel pending work; **does not** revoke grants |
| `GetGrants` / `RevokeClient` | See and remove access from the shell (no `SetGrant`) |
| `GetEnabled` / `SetEnabled` | Kill switch / safe mode: when false, `Action` and `RequestAccess` fail closed; the rest of the desktop keeps working |

Signals: `Changed` (grants, enablement, and every new audit row, emitted by the broker's `onAudit`), `PendingChanged` (queue depth). Never poll.
Trusted surfaces must still sanitize every field before display.

## Inference service

Separate process, `helix-inference`, bus name `org.helix.Inference`, D-Bus activated, unloads models after
idle. Embeds llama.cpp (and whisper.cpp for speech) as libraries; we do not write an engine.

### Sandbox

A systemd user unit with `PrivateNetwork=yes`, `ProtectSystem=strict`, `ProtectHome=read-only` plus a model
directory bind, `NoNewPrivileges=yes`, `SystemCallFilter=@system-service`, `MemoryDenyWriteExecute` off only
if the GPU stack requires it, and `DeviceAllow` limited to the render node. Model files are untrusted
binary input (GGUF parsers have had memory-safety bugs); the sandbox is the containment.

### Scheduling and budgets

- One resident model per slot with prefix and KV-cache reuse for the system prompt.
- Priorities: `interactive` (launcher, assistant) preempts `background` (summaries, indexing).
- A user-visible memory budget (RAM and VRAM). Under compositor load or on battery the service backs off:
  smaller context, CPU fallback, or refusal with a readable reason. It reads the power profile the shell
  already controls and never starves helix-niri's liquid-glass renderer of the GPU.
- `GetSnapshot` reports backend (Vulkan/CPU first), loaded models, budget, queue depth.

### Reliable tool calls

The Assistant's small local model emits tool calls under a **grammar generated from `schema-json`**
(GBNF): command names and enum arguments are the only legal tokens. This removes most malformed-call
failures without a bigger model. Larger or cloud models are an explicit escalation, a visible grant, never
a silent fallback.

### Models and compatibility

- Store under `~/.local/share/helix/models/`, hash-verified, quota-limited. No weights in the OS image.
- First-party clients use `Inference1`. Third-party apps get an OpenAI-compatible endpoint on a Unix
  socket, behind the same grants. Existing `desktop-ai-*` and `ai-usage-*` commands become clients.

## Implementation status

`Agent Surface` is registered by `helix-service` (`agent_endpoint`, `agent_broker`, `agent_policy`) and covered by an
end-to-end test (`tests/desktop_agent_integration.py`). The Rust `helix-desktop` scaffold passes the same integration
tests (`tools/nested/helix-desktop-agent-integration.sh`, CI job `helix-desktop-agent`). It needs `msg-risk.toml` (installed to
`<datadir>/helix/`, or `HELIX_RISK_POLICY`); without it every method fails closed with the reason. Caller identity
is the verified executable, plus the script path for interpreters, so two Python agents do not share grants.
Known weak spot: an arbitrary binary named `helix-settings` could claim the manager role; sandboxing agent
runners is the real fix (see the limit above). The C++ shell registers as confirmer and shows `agent-confirm`
(`cpp/desktop/` confirmation queue): a queue (sanitized text, at most 8 pending and 3 per client, refused beyond that) and a panel
where Enter never approves, Escape denies, Allow stays disabled for 700 ms after each new request, grant tiers
default to least privilege and closing the panel denies what is pending. It was checked in a nested helix-niri
session with isolated state: a real access request opened the dialog and closing it denied the request. Not yet
checked: the disabled look of Allow during the arming window, clicking Allow/Deny with a pointer, and the
confirm-tier action layout. The same queue and panel rules are ported in Rust as
`helix-shell-core::{ConfirmationQueue, Confirmer}` (mutation-checked) and `helix-desktop-api::ConfirmerClient`
(RegisterConfirmer / Confirm / ConfirmationRequested / ConfirmationEnded only). The Tauri Overlay confirmer surface is wired behind `HELIX_SHELL_CONFIRMER=1` (nested
`tauri-shell.sh` sets it; the live trial must not). It is a separate full-output Overlay window
(not a panel in the shell page), created on demand and destroyed when the queue is empty, with
exclusive keyboard. Text reaches the page only as a sanitized `ConfirmerView` (`textContent`).
Arming starts when the page is ready (`present()`), not when the D-Bus signal arrives ahead of
the webview, so Allow is not already live after a slow load. Nested e2e
(`tools/nested/assistant-flow-test.sh`, which runs `confirmer-test.sh`) against real Agent Surface with a real pointer:

1. Enter never approves; Escape denies → `the user declined`.
2. Pointer Deny → declined.
3. Pointer Allow after `CONFIRMER armed` on a grant with `observe`+`confirm` → granted
   `["observe"]` only (`confirm` stays unchecked).
4. Client `Cancel` → `ConfirmationEnded` destroys the Overlay; client sees cancelled.

Measured (release, nested): confirmer-open shell USS ~195 MB (PSS ~273 MB without
helix-service); after destroy ~142 MB USS. Under the 320 MB USS budget. Gotchas: wait for
`NameHasOwner` before any Desktop call on the private bus, and use `NO_AUTO_START` in test
clients — otherwise dbus activates the installed service with the daemon's real HOME; the
WebKit reap after destroy must only kill PIDs that existed at destroy time or it kills the
next confirmer's web process; confirmer layout is written to `layout-confirmer.json` (shell
`layout.json` must not overwrite it); nested tests run
`target/release/helix-shell-tauri` so release builds are required. The MCP adapter now uses `Agent Surface` (see `tools/helix-agent/README.md`). Settings has an **Agents** page (manager
role): apps with access, per-tier switches, remove, and recent activity. It was checked in a nested session against
seeded hostile data (markup, bidi controls, newlines in names and arguments): all rendered inert. App names shown
to the user come from the verified identity only (`friendlyClientName`: ASCII letters, digits and `._+-`, everything
else `_`), and a self-declared label appears only as "it calls itself". Not yet checked: toggling a switch or
pressing Remove with a pointer, and the unavailable state.

## Implementation status: agent actions under the Tauri shell (2026-10-08)

Until now an approved action was carried out by sending `helix-panel` commands to the **native** shell's socket, so under the Tauri shell
every action failed with "helix is not running". The Tauri shell now serves that socket (`helix-<WAYLAND_DISPLAY>.sock`, mode 0600,
`shell/helix-shell-tauri/src-tauri/src/agent_ipc.rs`) and understands exactly the commands in `helix_shell_core::agent_msg::SUPPORTED`
(volume, brightness, theme mode, night light, do not disturb, open and close its own panels). Every other command gets
`error: unknown command (this shell does not support it)`. The line is parsed by a pure, mutation-tested model (whole numbers in range,
listed words only, no extra arguments, no control characters, 1 KiB limit) and executes through the code the sliders and key bindings use.
A test checks that each supported command is classified in `msg-risk.toml` and none is `blocked`.

The shell also writes `helix-shell-tools-<display>` (first line its pid, then the command names). `ListTools` offers only those names while that
shell is alive (a file whose pid is gone is ignored), so the model is not offered `bar-toggle` or `wallpaper-next` under a shell that cannot
do them; with no live list every classified command is offered, as for the native shell. Socket choice no longer trusts the newest
`helix-wayland*.sock`: sockets that nobody answers on (left by tests or a crashed shell) are skipped.
Tests: `tools/nested/agent-ipc-test.sh` (the wire protocol, effects and refusals), `tests/desktop_agent_integration.py` steps 12 to 13
(tool list, stale list, dead socket). `HELIX_AGENT_IPC_DIR` moves both files for tests; nested sessions always set it.
Accepted limit, as for `org.helix.Shell1`: the socket does not authenticate other processes of the same user; Agent Surface is what gates agents.
Also supported now: `status` (observe: the desktop's state, windows by id and application name only, **never a title or any other program text**: `agent_status.rs`),
`workspace-switch next|prev` (act) and `window-close <id>` (confirm: only a window the shell knows can be named, never "the focused one", which can change before Allow; the approval dialog says in the
shell's own words which window the id is, `describe_window_close`, and a gloss can never come from the request). The window's application name comes from
the desktop entry when the window really is that application (`identity.rs`), otherwise it is the sanitized program name and `verified` is false.
Also supported now: `app-launch` (confirm: exact desktop id, exact name, or a single unambiguous search hit from the shell's catalog),
`clipboard-text` (observe-private), `clipboard-copy` / `clipboard-clear` (confirm; clipboard text is never logged).
Chat → act bridge: `org.helix.Shell1.AssistantSubmit` opens the bar Assistant and runs the same path as typing there
(Mobius Act). Not yet reachable: focusing a window by id, media, files, notification-show.

### Measured: a shell that carries out only some of the tools (2026-10-08)

The Tauri shell carries out 22 of the 115 tools the evaluation offers. How the assistant should treat the rest was measured (7B, flat + decline,
`tests/inference_eval.cpp --tools <list>`, tuning set 92 requests and the held-out 60; "must decline" counts every request whose answers are all
outside the shell's 22):

| What the model is shown | Actionable exact (tuning / held-out) | Declined correctly (tuning / held-out) | Acted instead of declining |
|---|---|---|---|
| all 115 tools (the old behaviour; unsupported ones fail at the shell) | 99% on 82 (only 28 are possible) | n/a | n/a |
| only the 22, 5 examples | 100% / 82% | 40 of 64 / (not run) | 24 / (not run) |
| only the 22, 9 examples that show declines, plus a sentence to decline unless exact | 100% / 82% | 50 of 64 / **16 of 43** | 14 / **27** |
| **all 115, and a choice outside the 22 becomes a decline** (what ships) | 93% / 65% | **64 of 64 / 42 of 43** | **0 / 1** |

Offering only the shell's tools makes the model choose a similar-sounding one ("turn the mic down" became `volume-down`, "get rid of the dock" became
`panel-close`), and prompt wording that fixed the tuning set did not carry over to the held-out set (16 of 43). Showing the whole list and turning
a choice the shell cannot carry out into a decline is far safer (a refusal costs a sentence; a wrong action changes something) at the price of
actionable accuracy: some requests the shell *could* do are answered with a tool it does not have ("open preferences" became `settings-open`,
"it's too loud" became `mic-volume-down`). That is a list of commands worth teaching the shell next (`settings-open` first). The assistant does this
in `assistant_main.cpp`; the service's `ListTools` and `--execute` apply the same list. The eval's `--tools`, `--map-unsupported` and
`supported_examples` reproduce all four rows.

## Implementation status: helix-inference

`helix-inference` is a separate, D-Bus-activated process (`org.helix.Inference`, interface `org.helix.Inference1`,
contract in `protocols/dbus/org.helix.Inference.xml`). The queue (Rust `services/helix-inference`), the interim C++ engine (`llama_backend`) and the
D-Bus layer (`inference_service`) are built and tested; `tests/inference_service_integration.py` drives the service with a
scripted backend over a real bus (streaming, replies only to the requester, strict arguments, per-client limit, cancel,
cleanup when a caller disappears) and `tests/inference_llama_test.cpp` runs a real model.

- **Sandbox.** `assets/services/helix-inference.service.in` carries 20 hardening properties (no network, no writes
  outside private temporary space, read-only home, restricted system calls, no capabilities, `AF_UNIX` only).
  `tools/check-inference-sandbox.py` applies exactly those properties to transient units and confirms the network and home
  writes are blocked, the model directory is readable, and the real service still sees the GPU and runs a model.
- **Caller identity differs from Agent Surface.** Any hardened user unit runs in its own user namespace, where another process's
  `/proc/<pid>/exe` cannot be read. So this service identifies callers by `pid:<n>` from the bus and uses that only for
  per-caller limits. Ownership of a request is the caller's unique bus name. There are no per-program grants yet: any
  same-user process may use the model, within the queue limits. Per-client grants for inference belong with the Settings
  Agents page.
- **Memory.** The model is freed after `HELIX_INFERENCE_IDLE_SECONDS` (default 300) of inactivity, never while a request is
  running or waiting. `GetSnapshot` reports the compute devices and their free memory.
- **Not done:** startup preload and warm-up, a VRAM budget that backs off when the compositor needs the GPU, KV-cache reuse
  of the shared prompt, the evaluation suite, packaging the engine (it is built by hand into `third_party/`, and the
  installed binary's library path points there), and a client in the assistant that uses it.

## Implementation status: helix-assistant

`helix-assistant "<request>"` (`services/helix-assistant`, Rust; the logic is `helix-shell-core::agent::assistant_core`). Plain English in, one desktop action out.

1. `Hello` to Agent Surface to learn this client's grants. With none, it asks once (`RequestAccess observe, act`); the shell
   shows the usual approval dialog. With no shell running it stops: a headless session cannot grant itself access.
2. The tool list and grammar are built from the grants (`observe`, `observe-private`, `act`), the same measured
   configuration as the evaluation (flat catalog, decline tool, 5 examples, tool hints), and sent to helix-inference.
3. The reply is parsed strictly. `decline` is shown and never forwarded, whatever arguments came with it.
4. Reading runs at once. Anything that changes the desktop is shown first as the tool's plain-words description plus
   the exact call (`Step to the next power profile (power-cycle next)`), and runs only after a typed `y`. No terminal or
   no answer means no. `--dry-run` shows the plan and stops.
5. The call goes through Agent Surface like any client's: grants, confirmations, taint and audit all still apply. Results
   from outside sources are labelled.

The plain-words description in step 4 exists because of a real result: the model answers "Restart my laptop" with
`power-cycle next`, which sounds like a reboot and only changes the power profile. Showing the tool's name alone would
have misled the person.

Tested with scripted model replies (`assistant_integration`: first-use grant, preview, no/empty/yes answers, dry run,
decline, blocked and invented tools, garbage reply, audit) and mutation-checked (running changes without asking, or
skipping the ask, fails the tests). Tried live against the 7B model in an isolated session: about 0.5 s per request once
warm, 25 s for the first request (model load and GPU kernels).

Model choice: the person's `HELIX_ASSISTANT_MODEL` if installed, else the largest installed model (the 7B in the
measurements). `helix-assistant --warm` loads it and primes its prompt cache with a background request, so the first real
request takes about 0.7 s instead of 25 s (measured live). `assets/services/helix-assistant-warm.service.in` runs it at
login; it is not installed or enabled by default, and the model is freed again after the idle timeout (300 s).

Packaging: `tools/install.sh` puts the engine's runtime libraries in a private `<prefix>/lib64/helix` (`tools/scripts/install_engine.py`)
and gives `helix-inference` an install rpath to it, so nothing system-wide changes and the repo's `third_party/` is
not needed at run time. `tools/install-assistant.sh <staged tree>` installs only the model runtime, the assistant and
`helix-service` (backing up the old one), restarts nothing, and has `--uninstall`. Installed on the development machine
2026-10-06: the service activates under its sandbox and lists both models.

### The shell panel

`helix-panel panel-toggle assistant` opens the assistant panel (bind it to a key in the compositor config).
It is a thin face over `helix-assistant --json`, which prints one JSON object per call (`done`, `confirm`, `explain`,
`error`). The panel never talks to the model and holds no authority: grants, confirmations and audit belong to the
`helix-assistant` client identity, so a broken or closed panel cannot widen anything.

- Reading runs at once. A change shows "About to: ..." with Cancel and Do it; Do it runs
  `helix-assistant --json --request <text> --execute <tool> <args>`, exactly what was shown, and `--execute` re-checks the
  tool against the grants like a model reply (blocked, invented and `decline` are refused).
- Enter never confirms (focus stays in the input, and a submit while a change is waiting is ignored), Do it is disabled
  for 700 ms after it appears, Escape backs out of a pending request before it closes the panel, and opening the panel
  always starts fresh.
- The state machine (`core/helix-agent/src/assistant.rs`) has no UI in it and is unit tested, including late answers
  after cancel, a second yes, malformed output, and display-safety of model text. Mutation-checked.
- Tried in a nested niri session on a private bus with the real 7B model: typing, "Thinking...", the confirmation, two
  stray Enter presses (nothing ran, audit empty), keyboard-driven Do it (ran through Agent Surface, audited as allowed), the
  honest failure when there is no backlight, and focus returning to the input. Mouse clicks were not tried.

The confirmation shows the tool's plain-words description, which for near-neighbours includes the disambiguation written
for the model ("display brightness, not sound"). Slightly wordy for a person; a separate user-facing line is a possible
refinement.

Not done: a VRAM budget alongside other GPU users, multi-step requests, and
`confirm`-tier tools (not offered to the model).

## Measured: model quality and speed

`tests/inference_eval.cpp` runs 92 realistic requests (`tests/eval/desktop_requests.json`: 82 to act on across 10 families,
10 that must be declined) against a real model through the whole stack: queue, grammar and the live command schema, with
the read-only, everyday and `act` tiers (115 tools). Accepted answers are listed per request, and accepted arguments
default to none, so a hallucinated argument is not scored as correct. `tools/run-inference-eval.py` runs the matrix. RTX 4080
Laptop GPU, Vulkan, greedy decoding, one run per cell, warm-up excluded; roughly +/-3 points of noise on 82 requests.

| Model | Configuration | Exact | Right tool | Declined correctly | Acted instead of declining | Median latency |
|---|---|---|---|---|---|---|
| 1.5B | flat | 61% | 68% | 0 / 10 | 10 | 0.83 s |
| 1.5B | flat + decline | 62% | 72% | 0 / 10 | 10 | 1.0 s |
| 1.5B | staged + decline | 63% | 72% | 1 / 10 | 9 | 0.88 s |
| 1.5B | flat + decline + 5 examples | 70% | 80% | 1 / 10 | 9 | 0.89 s |
| 1.5B | staged + decline + 5 examples | 60% | 72% | 1 / 10 | 9 | 0.86 s |
| 7B | flat | 85% | 96% | 0 / 10 | 10 | 2.2 s |
| 7B | flat + decline | 87% | 96% | 6 / 10 | 4 | 2.3 s |
| 7B | staged + decline | 72% | 84% | 5 / 10 | 5 | 1.7 s |
| 7B | **flat + decline + 5 examples** | **90%** | **95%** | **7 / 10** | **3** | **2.1 s** |
| 7B | staged + decline + 5 examples | 78% | 83% | 5 / 10 | 5 | 1.6 s |

What it says:

- **Validity is a guarantee; correctness and safety are not.** Every reply in every run was a valid tool call. Nothing about
  validity makes a call safe or right.
- **A small model does not know what it cannot do.** The 1.5B model declined 1 of 10 impossible requests. For "reboot the
  computer" it set the power profile to performance, and for "shut down the PC" it cycled the power profile: real state
  changes from a valid call. The 7B model's wrong actions on the same requests were reads (`wifi-status` for "turn off
  Wi-Fi") or a mild one (`media raise` for "open Spotify"), and it declined 7 of 10 correctly with the examples. Any
  assistant must show the user what it is about to do for `act`-tier calls, not run them silently.
- **A decline option is needed and works with the larger model** (0/10 to 6-7/10 correct declines). It barely helps the 1.5B.
- **Flat beats two-stage for the larger model** (87-90% against 72-78%). The two-stage group choice is where it goes wrong;
  its only advantage is latency (1.6 s against 2.1 s). An earlier 14-request test suggested the opposite, which is why the
  larger suite exists.
- **Five examples help modestly** (+8 points for the 1.5B, +3 for the 7B), at no latency cost.
- **Remaining 7B misses** (95% right tool): "make it louder" and "quieter" became `brightness-up`/`brightness-down`;
  "show my clipboard history" became `notification-history`; "status of the desktop" became `desktop-ai-status`; and four
  right-tool answers carried empty or extra optional arguments (`bar-hide [""]`, `brightness-set ["", "60"]`).
- **Latency.** About 2 s per request for the 7B, dominated by reprocessing the roughly 2,000-token tool list each time.
  Reusing the key-value cache for the shared prompt prefix should cut most of it.

### Follow-up experiments (7B, flat, decline, 5 examples)

| Change | Exact | Right tool | Median latency |
|---|---|---|---|
| Before | 90% | 95% | 2.1 s |
| Prompt-prefix cache (the shared tool list is not reprocessed) | 90% | 95% | **0.62 s** (98% of prompt tokens served from memory) |
| Reason written before the tool (hypothesis: a short chain of thought) | 48% | 61% | 2.7 s |

- **The prompt cache is kept.** The context outlives a request and only the part of the prompt that changed is processed. It
  is 3.4 times faster at the same accuracy. `tests/inference_llama_test.cpp` checks that a cached run gives exactly the same
  answer as a cold one, and that trusting a stale cache is caught.
- **Reason-first is rejected.** With room to talk first, the model reasons itself out of acting ("the request is vague...")
  and wrongly declines 26 requests. The default stays tool, then arguments, then an optional reason. The option remains in
  the grammar (`GrammarOptions::reasonFirst`) so the result can be reproduced.
- **Non-empty free-text arguments are kept** (an empty string is never a meaningful value), but they did not change the
  score: the model now writes placeholders for optional arguments (`["bar-name"]`, `["<value>", ">60"]`) because it copies
  the signature it was shown. The leverage is in not offering optional free-text arguments the model should not fill.

Ideas to test next, in order of expected value: drop optional free-text positionals from what is offered (or cap a tool's
arguments at its required ones unless the optional one is an enum); evaluate a 14B-class model; tell the model which of two
near-neighbours is which ("volume" is sound, "brightness" is the screen) in the tool summaries.

Earlier smoke test (14 requests, kept for the record): both models 14/14 valid; right tool 7/14 (1.5B) and 11/14 (7B).
A constrained reply cut off by the token limit is an error, never returned as an answer. One unexplained slow run
(about 6.7 minutes instead of about 20 s) did not reproduce; probably GPU contention.

### Argument rules and tool hints (7B, flat, decline, 5 examples, warm cache)

Two changes to what the model is shown. Neither changes what a tool may do.

1. Only required arguments, enum arguments and `context` of panel/settings commands are offered. Optional free text
   (a file path, a message) used to be offered and the model filled it with invented values. Strings are length-bounded
   in the grammar, which also ends the runaway string reply seen once.
2. `protocols/ipc/tool-hints.toml` replaces a command's `--help` summary with a one-line description in a person's
   words, written for whole confusable families (sound vs screen brightness, desktop state vs AI state, panel vs history).

| Set | Before | Argument rules | + hints |
| --- | --- | --- | --- |
| Original, exact | 90% (98% after earlier argument fix, see above) | 80/82 (98%) | 81/82 (99%) |
| Held-out, exact | 44/52 (85%) | 46/52 (88%) | 47/52 (90%) |
| Held-out, right tool | 48/52 | 47/52 | 49/52 (94%) |
| Held-out, declined correctly | 3/8 | 3/8 | 3/8 |

Read the held-out numbers with care: the hints were written after seeing which near-neighbours failed, so the held-out
set is no longer fully independent. The honest claim is "about 90% exact on requests of this kind", not a precise figure.
Declining is the weak spot and hints did not move it: 5/8 impossible requests still get a nearby tool, including
`power-cycle next` for "Restart my laptop" and `session lock` for "Log me out". That is why the assistant client must
show every state-changing call before running it and rely on the Agent Surface tiers, never on the model refusing.

## Build order

1. Contracts and tests (done: drafts, contract test, `msg-risk.toml`, `schema-json`).
2. `Agent Surface` in helix-service: identity, grants, audit, `Action` for `observe`/`act` tiers. Point the MCP
   adapter at it; keep `confirm` refusing until step 3.
3. Shell confirmer dialog and the taint rule; then enable `confirm`.
4. Generate MCP tools and the GBNF grammar from `schema-json`.
5. `helix-inference` with `chat`, grammar-constrained, one model, CPU and Vulkan, sandboxed.
6. Eval suite (15 to 20 desktop tasks) gating changes to prompts, grammar and model choice.

## Open questions

- How is the manager role authenticated? Proposal: exe path equals the installed `helix-settings`, verified
  from `/proc/<pid>/exe`, plus Flatpak app id where applicable.
- Does `Agent Surface` stay on `org.helix.Desktop` or move to its own bus name so it can be sandboxed separately?
- A11y tree observation (AT-SPI) tier: `observe-private` by default?
- Per-window `agent-visibility` rule in helix-niri so capture can exclude password managers and secrets.

### The Agents panel in the Tauri shell (workstream 2)

`helix-desktop-api::ActivityClient` wraps `AgentSurfaceActivity1` (a contract test pins the method list to the XML, and a second test
keeps `SetGrant`, `RegisterManager` and every client verb out of it). `helix-shell-core::activity` is the pure model: every field an
agent can influence is sanitized, a tier that is not exactly one of the five names is shown as `unknown` (sanitizing could otherwise
turn `ob<bidi>serve` into a real tier name), `stop` is refused unless the last snapshot listed that operation, `revoke` unless it
listed that client, and `stop_all` is never refused. The panel (`agents`, also `org.helix.Shell1.OpenPanel("agents")`) shows what is
waiting with Stop, who has access with Revoke, the kill switch, a Stop-all button and the recent audit rows; the bar button reads
"Agents", "Agents · N waiting" or "Agents off" and is hidden when the shell is not the registered desktop.

Constraints worth knowing:
- Only the **connection that called `RegisterDesktop`** may use `AgentSurfaceActivity1`, so the shell uses that one connection (set by the
  confirmer once registered), not its shared one. The live trial runs beside the C++ shell, which holds the role, so the panel is
  enabled by the same opt-in as the confirmer (`HELIX_SHELL_CONFIRMER`) and the trial shows nothing.
- While a confirmation is on screen its Overlay covers the output, so the panel's Stop is reachable for requests queued behind
  it or when the dialog was dismissed elsewhere; the nested test drives Stop through a test-only file hook that runs the same path.
- The kill switch is held in the service's memory: a restart turns agents back on. Operations already running are not stoppable yet.

Verified nested (`tools/nested/activity-test.sh`, real service, real pointer, a stand-in agent `agent-probe.py`): grant through the
confirmer, an allowed action in the history, kill switch off refuses with "agent access is turned off" and on restores it, Revoke
makes the next request fail with "no observe grant", Stop cancels a waiting request and closes its dialog. Shell memory with the
panel open in the nested session: USS about 119 MB (PSS 172 MB without the service).

### Confinement is a deployment requirement

`org.helix.Shell1` (panels, volume, brightness, do-not-disturb, night light, dark theme) is served to the whole session bus: it does what a
person's own key binds do and carries no grant check. An agent that can reach the session bus can call it, so agent processes must be confined
(sandbox with no session-bus access except `org.helix.Desktop.AgentSurface1`), not merely trusted to use Agent Surface. The model service already runs sandboxed;
the assistant and any third-party agent must be packaged the same way before they are treated as untrusted-but-contained.
