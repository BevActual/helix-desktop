# Mobius as an Agent Client Protocol client

Status: 2026-10-18. Steps 1 and 2 of the build order are built (the `helix-acp` crate and its fake agent); the Mobius side is not. Sources: the protocol's published schema and documentation (agentclientprotocol.com and its repository, Apache-2.0), and this computer's installed agents (`opencode`, `hermes`, `codex`, `claude`, `cursor-agent`), whose help output was read. No agent's stored credentials were opened.

## Why

Mobius's Tools mode and Code stage run one loop of Mobius's own. Other coding agents are better at some work, and the person may already have signed in to them (including with subscriptions that a third-party app is **not** allowed to use, see `mobius-settings.md`). The **Agent Client Protocol (ACP)** lets Mobius drive such an agent as a subprocess, while keeping the two things that matter to Helix on the Mobius side:

* **The person's files and terminal stay Mobius's.** In ACP the *client* owns the file system and terminals; the agent asks for each read, write and command. So every edit can go through Mobius's reviewed, undoable path (`edits.rs`) and every command through its sandbox (`terminal.rs`), whichever agent is asking.
* **The agent keeps its own login.** Mobius never reads, copies or proxies the agent's tokens. If the person signed the agent in with a subscription, that is between them and the agent's maker; if the maker forbids it, the agent would not offer it.

## The protocol, as published

* **JSON-RPC 2.0 over the agent's standard input and output**: the client launches the agent as a subprocess; messages are single lines ending `\n` with no embedded newlines; anything on `stdout` that is not a message is a violation; `stderr` is free-form logging. (A streamable-HTTP transport is a draft.)
* **Client → agent**: `initialize` (protocol version, `clientCapabilities`: `fs.readTextFile`, `fs.writeTextFile`, `terminal`, …), `authenticate`, `session/new` (a working directory, optional extra directories, MCP servers), `session/load`, `session/prompt`, `session/set_mode`, and the notification `session/cancel`.
* **Agent → client**: the notification `session/update`, and requests the client must answer: `session/request_permission`, `fs/read_text_file`, `fs/write_text_file`, `terminal/create`, `terminal/output`, `terminal/wait_for_exit`, `terminal/kill`, `terminal/release`, `elicitation/create` (a question for the person).
* **Updates**: message chunks (`agent_message_chunk`, `agent_thought_chunk`, `user_message_chunk`), `tool_call` and `tool_call_update` (a kind: read, edit, delete, move, search, execute, think, fetch, switch_mode, other; a status: pending, in_progress, completed, failed; file locations), `plan`, `available_commands_update`, `current_mode_update`, `usage_update`, `notice`, compaction updates.
* **Permission** is a request with options of kind `allow_once`, `allow_always`, `reject_once`, `reject_always`; the client may answer from its own settings.
* **Content** is `text`, `image`, `audio`, `resource_link` or an embedded `resource`. A turn ends with a stop reason: `end_turn`, `max_tokens`, `max_turn_requests`, `refusal` or `cancelled`.

## Which agents on this computer speak it

| Agent | ACP |
|---|---|
| `opencode` | Yes: `opencode acp` ("start ACP server") |
| `hermes` | Yes: `hermes acp` |
| `codex` | Not itself; Zed publishes a `codex-acp` adapter. Codex's own `app-server` is a different protocol |
| `claude` | Not itself. Zed's adapter is built on Anthropic's Agent SDK, which falls under the **subscription rule**: usable with an API key, not with a Pro/Max login |
| `cursor-agent`, `agent`, `agy` | No ACP option found in their help |

So the first useful targets are OpenCode and Hermes, plus any agent the person adds by command.

## How it fits Helix

| Concern | Design |
|---|---|
| **Edits** | `fs/write_text_file` is not applied. It becomes a proposed change in the existing reviewed-edit flow (`edits.rs`): a diff the person accepts, rejects or undoes, with restore points. `fs/read_text_file` is limited to the project folder and the files the person attached, and refuses control files and anything outside |
| **Commands** | `terminal/create` runs in `terminal.rs`'s bubblewrap (no network, no home, project at `/workspace`), after the person approves, as Tools mode does. Output is streamed back through `terminal/output` |
| **Permissions** | `session/request_permission` is shown on Mobius's approval card with the agent's options. `allow_always` is stored **per project and per agent** and listed in Settings, and can be revoked. Desktop control (the Agent Surface) is never granted to an ACP agent |
| **The agent process** | It runs with the person's privileges, so it is **confined too**: bubblewrap with the project folder read-write, the agent's own config and credential folder (and nothing else of the home folder) visible to it, network allowed because the agent must reach its model, no desktop sockets, no D-Bus session. Whether each agent works under this is tested, not assumed; an agent that cannot be confined is offered only with a plain warning |
| **Output** | Agent text goes through the same Markdown renderer as any reply (no HTML, no remote images). Tool-call titles and paths are untrusted text. Thoughts fold into the existing "Thought it through" row |
| **Credentials** | Never read. If an agent needs `authenticate`, Mobius shows the methods the agent lists and lets the agent drive its own browser or terminal sign-in. A key the person types is handed to the agent's process environment for that session only and is not stored by Mobius unless the person chooses |
| **MCP servers** | `session/new` carries none at first. Passing Mobius's own tools to an agent is a later, explicit choice |
| **Cost** | `usage_update` shows what the agent reports; Mobius adds none of its own |

## What the person sees

* **Settings → Agents** (a new section): installed agents found by name (`opencode`, `hermes`), a way to add any command that speaks ACP, a **Test** button that runs `initialize` and shows the agent's name, version, capabilities and sign-in methods, and a **confinement check** result.
* **The Code stage** gains **Run with ▾**: Mobius's own loop, or an agent. An agent session is a chat thread: its message and thoughts stream in, each tool call is a row with its kind, status and files (click to open the file), its plan is a checklist, edits arrive as diffs to review, and commands as approval cards.
* **Stop** sends `session/cancel` and waits for the agent to answer with `cancelled`.

## Threats and answers

| Threat | Answer |
|---|---|
| The agent edits something outside the task | It cannot: writes are proposals inside the project folder, and the process is confined |
| The agent runs a destructive command | The command is approved first, and runs in the sandbox with no network and only the project mounted |
| A malicious or compromised agent binary | It is confined as above, runs only when the person starts it, and is one the person chose by name or command. Mobius does not download agents |
| Prompt injection through project files or web pages the agent reads | The agent's tools are mediated; the worst a poisoned file can ask for is another approval card, which names the command and the files |
| The agent spams the interface | Updates are rate-limited and the thread is bounded; the session can be stopped and killed |
| An agent writes forged protocol messages to `stdout` | A line that is not valid JSON-RPC ends the session with an error shown to the person |
| Credentials leak into transcripts | Mobius stores the thread, not the agent's environment; the agent's own files are outside what Mobius reads |
| An agent asks to read the person's secrets (`~/.ssh`) | `fs/read_text_file` refuses anything outside the project and attachments, and the process cannot see those folders |

## Build order

1. **`helix-acp` crate** (`core/`): newline-framed JSON-RPC over a child process's pipes; the subset above as typed messages; unit tests over in-memory pipes, and tests that check our messages against the published JSON schema. Written from the specification (Apache-2.0; no code copied). Whether to use Zed's `agent-client-protocol` Rust crate instead is open: it is Apache-2.0 and so compatible with MIT, but it is not in the offline registry here, and the subset is small enough to own.
2. **A fake agent** (a script) that speaks the protocol and misbehaves on request (bad lines, huge output, a request outside the project, no answer to cancel), for the nested tests.
3. **Mobius side**: the session driver, mediated file and terminal calls (reusing `edits.rs` and `terminal.rs`), the approval cards, the thread rendering. Nested tests drive the fake agent through a whole turn.
4. **Settings → Agents**, the **Test** and confinement check, and **Run with** in the Code stage.
5. **Confining the agent process**, measured against `opencode` and `hermes` by hand (they need accounts, so these runs are the owner's).
6. Later: `session/load` for resuming, modes, passing MCP servers, and the draft HTTP transport.

## Built so far

`core/helix-acp`: newline-framed JSON-RPC over a child process (`Client::spawn`), the subset of messages above as typed data (anything else is kept as raw JSON, so a newer agent is not an error), and a `Handler` trait through which the host answers every file, terminal and permission request. Safeguards, each tested against `tests/fake_agent.py`:

* a line that is not JSON, not JSON-RPC 2.0, or longer than 4 MiB ends the session with a protocol error;
* at most 16 of the agent's requests are answered at once, and the rest are refused ("Too many requests at once");
* the event queue is bounded, so a flood of updates (3000 in the test) slows the agent instead of growing memory, and none are lost;
* an agent that dies mid-turn is an error at once, not a hang; a request the crate does not know is answered "method not found";
* an agent on another protocol version is refused at `initialize`;
* `RefuseAll` is the default handler: a host that offers nothing is asked for nothing.

`tests/protocol.rs` also checks what the client sends (`initialize`, `session/new`, `session/prompt`, `session/cancel`, and its answers to permission and file requests) against the **published v1 schema** (vendored in `core/helix-acp/schema/`, Apache-2.0), with a small checker for the subset of JSON Schema that file uses, and checks that the checker can fail.

## Not doing

* Reading any agent's stored login, or reusing a subscription's token in Mobius.
* Letting an agent drive the desktop (the Agent Surface stays Mobius's own, behind the confirmer).
* Downloading or installing agents for the person.
* Building a Claude subscription route; an API key, or Claude through OpenRouter, remain the ways.

## Open questions

* **Does each agent work inside the confinement?** Some want to write caches or read global config; the sandbox may need a short, per-agent list of paths. This is the main unknown and decides step 5.
* **Edits are proposals, but some agents expect `write_text_file` to have taken effect** before their next read. The mediated read must return the proposed content for a file with a pending change, and the thread must say clearly when a change is waiting for the person.
* **One session or several?** Start with one session per chat.
* **Where does the approval card live while the person is in another place** (the Image stage, another chat)? Likely the existing banner pattern.
