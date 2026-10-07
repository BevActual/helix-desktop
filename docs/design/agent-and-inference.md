# Agent Surface v2 and built-in inference

Status: **draft**. Contracts are in `protocols/dbus/drafts/` (`org.helix.Desktop.Agent1`,
`org.helix.Inference1`). Nothing here is implemented yet. `agentic-os.md` describes what ships today.

## Why a second design

v1 is `tools/helix-agent` (MCP) in front of `noctalia msg`. Its risk policy
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
route we hand to sandboxed agent clients; the human `noctalia msg` socket stays for the user's own tools.

## Agent1

Served by `helix-service` (D-Bus activated, runs without the shell). One interface, three roles:

- **Client.** `Hello` registers a label; identity is read from the connection (pid, `/proc/<pid>/exe`,
  Flatpak app id), not from arguments. `ListTools` returns the tools the caller may use, generated from
  `noctalia schema-json` plus `msg-risk.toml`. `Action(tool, args)` returns an operation id. `Completed`
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
`<state dir>/agent/grants.json` (mode 0600; `~/.local/state/noctalia/` until the rename) and applied only if the write succeeds. Tiers a client
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
arguments truncated, secrets never logged), tier, decision, outcome. Readable by the manager role only.

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

`Agent1` is registered by `helix-service` (`agent_endpoint`, `agent_broker`, `agent_policy`) and covered by an
end-to-end test (`tests/desktop_agent_integration.py`). It needs `msg-risk.toml` (installed to
`<datadir>/helix/`, or `HELIX_RISK_POLICY`); without it every method fails closed with the reason. Caller identity
is the verified executable, plus the script path for interpreters, so two Python agents do not share grants.
Known weak spot: an arbitrary binary named `helix-settings` could claim the manager role; sandboxing agent
runners is the real fix (see the limit above). The C++ shell registers as confirmer and shows `agent-confirm`
(`src/shell/agent/`): a queue (sanitized text, at most 8 pending and 3 per client, refused beyond that) and a panel
where Enter never approves, Escape denies, Allow stays disabled for 700 ms after each new request, grant tiers
default to least privilege and closing the panel denies what is pending. It was checked in a nested helix-niri
session with isolated state: a real access request opened the dialog and closing it denied the request. Not yet
checked: the disabled look of Allow during the arming window, clicking Allow/Deny with a pointer, and the
confirm-tier action layout. The same queue and panel rules are ported in Rust as
`helix-shell-core::{ConfirmationQueue, Confirmer}` (mutation-checked) and `helix-desktop-api::ConfirmerClient`
(RegisterConfirmer / Confirm / ConfirmationRequested / ConfirmationEnded only). The Tauri Overlay confirmer
surface and nested pointer e2e are not wired yet. The MCP adapter now uses `Agent1` (see `tools/helix-agent/README.md`). Settings has an **Agents** page (manager
role): apps with access, per-tier switches, remove, and recent activity. It was checked in a nested session against
seeded hostile data (markup, bidi controls, newlines in names and arguments): all rendered inert. App names shown
to the user come from the verified identity only (`friendlyClientName`: ASCII letters, digits and `._+-`, everything
else `_`), and a self-declared label appears only as "it calls itself". Not yet checked: toggling a switch or
pressing Remove with a pointer, and the unavailable state.

## Implementation status: helix-inference

`helix-inference` is a separate, D-Bus-activated process (`org.helix.Inference`, interface `org.helix.Inference1`,
contract in `protocols/dbus/org.helix.Inference.xml`). The queue (`inference_queue`), the engine (`llama_backend`) and the
D-Bus layer (`inference_service`) are built and tested; `tests/inference_service_integration.py` drives the service with a
scripted backend over a real bus (streaming, replies only to the requester, strict arguments, per-client limit, cancel,
cleanup when a caller disappears) and `tests/inference_llama_test.cpp` runs a real model.

- **Sandbox.** `assets/services/helix-inference.service.in` carries 20 hardening properties (no network, no writes
  outside private temporary space, read-only home, restricted system calls, no capabilities, `AF_UNIX` only).
  `tools/check-inference-sandbox.py` applies exactly those properties to transient units and confirms the network and home
  writes are blocked, the model directory is readable, and the real service still sees the GPU and runs a model.
- **Caller identity differs from Agent1.** Any hardened user unit runs in its own user namespace, where another process's
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

`helix-assistant "<request>"` (`src/assistant/`). Plain English in, one desktop action out.

1. `Hello` to Agent1 to learn this client's grants. With none, it asks once (`RequestAccess observe, act`); the shell
   shows the usual approval dialog. With no shell running it stops: a headless session cannot grant itself access.
2. The tool list and grammar are built from the grants (`observe`, `observe-private`, `act`), the same measured
   configuration as the evaluation (flat catalog, decline tool, 5 examples, tool hints), and sent to helix-inference.
3. The reply is parsed strictly. `decline` is shown and never forwarded, whatever arguments came with it.
4. Reading runs at once. Anything that changes the desktop is shown first as the tool's plain-words description plus
   the exact call (`Step to the next power profile (power-cycle next)`), and runs only after a typed `y`. No terminal or
   no answer means no. `--dry-run` shows the plan and stops.
5. The call goes through Agent1 like any client's: grants, confirmations, taint and audit all still apply. Results
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

Packaging: `meson install` puts the engine's runtime libraries in a private `<prefix>/lib64/helix` (`scripts/install_engine.py`)
and gives `helix-inference` an install rpath to it, so nothing system-wide changes and the repo's `third_party/` is
not needed at run time. `tools/install-assistant.sh <staged tree>` installs only the model runtime, the assistant and
`helix-service` (backing up the old one), restarts nothing, and has `--uninstall`. Installed on the development machine
2026-10-06: the service activates under its sandbox and lists both models.

### The shell panel

`noctalia msg panel-toggle assistant` opens the assistant panel (bind it to a key in the compositor config).
It is a thin face over `helix-assistant --json`, which prints one JSON object per call (`done`, `confirm`, `explain`,
`error`). The panel never talks to the model and holds no authority: grants, confirmations and audit belong to the
`helix-assistant` client identity, so a broken or closed panel cannot widen anything.

- Reading runs at once. A change shows "About to: ..." with Cancel and Do it; Do it runs
  `helix-assistant --json --request <text> --execute <tool> <args>`, exactly what was shown, and `--execute` re-checks the
  tool against the grants like a model reply (blocked, invented and `decline` are refused).
- Enter never confirms (focus stays in the input, and a submit while a change is waiting is ignored), Do it is disabled
  for 700 ms after it appears, Escape backs out of a pending request before it closes the panel, and opening the panel
  always starts fresh.
- The state machine (`src/shell/assistant/assistant_session.*`) has no UI in it and is unit tested, including late answers
  after cancel, a second yes, malformed output, and display-safety of model text. Mutation-checked.
- Tried in a nested niri session on a private bus with the real 7B model: typing, "Thinking...", the confirmation, two
  stray Enter presses (nothing ran, audit empty), keyboard-driven Do it (ran through Agent1, audited as allowed), the
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
show every state-changing call before running it and rely on the Agent1 tiers, never on the model refusing.

## Build order

1. Contracts and tests (done: drafts, contract test, `msg-risk.toml`, `schema-json`).
2. `Agent1` in helix-service: identity, grants, audit, `Action` for `observe`/`act` tiers. Point the MCP
   adapter at it; keep `confirm` refusing until step 3.
3. Shell confirmer dialog and the taint rule; then enable `confirm`.
4. Generate MCP tools and the GBNF grammar from `schema-json`.
5. `helix-inference` with `chat`, grammar-constrained, one model, CPU and Vulkan, sandboxed.
6. Eval suite (15 to 20 desktop tasks) gating changes to prompts, grammar and model choice.

## Open questions

- How is the manager role authenticated? Proposal: exe path equals the installed `helix-settings`, verified
  from `/proc/<pid>/exe`, plus Flatpak app id where applicable.
- Does `Agent1` stay on `org.helix.Desktop` or move to its own bus name so it can be sandboxed separately?
- A11y tree observation (AT-SPI) tier: `observe-private` by default?
- Per-window `agent-visibility` rule in helix-niri so capture can exclude password managers and secrets.
