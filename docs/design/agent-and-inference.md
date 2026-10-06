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
`~/.local/state/helix/agent-grants.json` (mode 0600) and applied only if the write succeeds. Tiers a client
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

Append-only JSON lines in `~/.local/state/helix/agent-audit.jsonl`: time, client, tool, argv (literal
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
