# Research: what Cursor and the ChatGPT desktop app do in settings, models and commands

Read on 2026-10-18 from the copies installed on this computer: **Cursor 3.24.9** (the settings pages' labels and descriptions, found in its workbench bundle) and the **ChatGPT desktop app 26.930** (an Electron app; its 46,000 interface strings are catalogued by id, which shows how its settings and commands are organised). Both are Electron, so their interface text is readable without a disassembler; Ghidra would only matter for the native `codex` binary, and OpenAI publishes that one's source. Only structure and ideas are recorded here, paraphrased; no code, prompts or wording is copied. Findings are ranked at the end by what they would do for Mobius.

## Settings

| Finding | Source | Mobius today |
|---|---|---|
| **The settings rail has headings** (ChatGPT: Personal, Coding, Integrations, Archived; 60+ sections beneath). Cursor has about 16 pages and puts models, voice, source control and cloud agents together. | both | Five sections with no headings; fine now, needed once there are more |
| **Agent defaults are plain-language choices, each option with a sentence**: approval policy, sandbox, network access, **web search as four levels** (off, cached index, indexed, live), **output detail** (concise, balanced, detailed), **reasoning summary** (auto, concise, detailed, none). | ChatGPT | Style overlays exist per chat; no global "answer detail", "show thinking" or web-access level |
| **Settings have scopes**: project, global and admin, shown as groups. | ChatGPT | Per-project instructions exist; per-project model choice does not |
| **Model features can be curated**: "choose which reasoning effort levels appear in model controls". | ChatGPT | The model menu is curated with pins (done) |
| **Keys reveal their fields**, and say "Secret saved" once stored. | Cursor | Done |
| **A network page**: the domains the app must reach, an allowlist for firewalls, a diagnostics button, an HTTP compatibility mode. | Cursor | Provider tests only. See proposal 2 |
| **Embedding model is a setting**, with a note that the index must be rebuilt to use another. | Cursor | One model, vectors tagged by model (a change is handled) |
| **Ignore patterns** for what the assistant may read or suggest in (glob list). | Cursor (Tab, indexing) | Knowledge folders skip hidden and build folders; no patterns |
| **Appearance**: separate light and dark themes (each with colours and font), theme import, translucent sidebar, reduce motion, content font size apart from interface font size, smart punctuation. | ChatGPT | Follows the system theme; no size or motion settings |
| **Conversation density** (how much detail tool calls show), chat width, text size, code-block wrapping, tips on the empty screen, system notifications "when the agent completes or needs attention", resume interrupted agents after a restart. | Cursor | Not set anywhere |

## Memory and privacy copy

* **Memory is two switches and a reset**: create memories from chats; and separately, from chats that used tools or web search. Reset asks "Delete all memories?" and says what it deletes. Mobius's memory is manual only; the equivalent step is an opt-in *suggestion* that is reviewed.
* **"Computer history" is the model for any future activity context**: off by default; says what it saves (text summaries, never the screen or audio), that private browsing is never included, and lets the person include only, or exclude, chosen apps and websites. Worth copying in tone and structure if Mobius ever reads what the person is doing.

## Model controls

* **One "power" dimension instead of a model plus an effort**: Light, Medium, High, Extra high, Ultra, Persistent, adjusted with the arrow keys, with "consumes usage limits faster" shown at the top end. A separate **Fast** switch says "N× speed, more usage". "Standard" and "Extended" reasoning. A **Default: recommended set** with Reset to default.
* **Recent combinations** (model, effort, speed) are a command of their own.
* **Retirement and upgrade banners**: "{model} is retiring; this conversation will switch to {model}" with Switch now and Dismiss.
* **A model that is not available is shown locked, with a way to unlock it**, not hidden.

## Commands

Every action in the ChatGPT app is a named command with a one-line description that the palette and shortcut editor share (about 140). Cursor's agent messages have **Steer / Interrupt / Queue**, with a setting for what a message sent mid-reply does by default. Worth adopting:

* *send in the background* (send without opening the chat), *steer* and *queue* as separate commands,
* *recent models*, *copy the chat as Markdown*, *copy the last code block*, *temporary chat*, *quick chat*, *next chat needing attention*, *find in this chat*, *undo / redo the last app action*,
* *show the shortcuts available right now* and *customise shortcuts*,
* *import from other AI apps* (Cursor: "Import Claude Code conversations", "include third-party plugins, skills and other configs"),
* approvals as commands with shortcuts (*approve*, *decline*).

## Ranked proposals for Mobius

| # | Proposal | Size | Why |
|---|---|---|---|
| 1 | **Power control**: Quick / Balanced / Deep in the composer, mapping to a small model, the default, and a thinking model, each saying its measured speed here; **recent models** at the top of the model menu | M | One decision instead of two; uses what Settings now knows (fit, speed, "thinks first") |
| 2 | **A network page**: the exact hosts Mobius may contact (huggingface.co when you press Get, openrouter.ai when you use or refresh cloud models, nothing otherwise), with why, and a button to test each | S | Makes the local-first claim checkable; nobody else shows it |
| 3 | **Answer settings**: detail level (concise, balanced, detailed) and how thinking shows (always open, folded, hidden) | S | Cheap, familiar, and the folded thinking row already exists |
| 4 | **Knowledge**: ignore patterns per folder; opt-in *suggested* memories from chats, reviewed | M | Cursor's ignore lists and the memory switches, in Mobius's review-first style |
| 5 | **Import chats** from a ChatGPT or Claude export file | M | Lets a person bring their history; local file, no account |
| 6 | **Messages while a reply runs**: steer, interrupt or queue, with a default | S | The queue exists; the choice and its setting do not |
| 7 | **Settings rail headings, shortcut customisation, a "shortcuts right now" view** | M | Pays off as sections grow |
| 8 | **A banner when the chat's model is gone or fails**, offering the next best | S | Cloud models retire; local files get removed |
| 9 | **Appearance**: content font size, reduce motion, theme import | M | Accessibility as well as taste |

## Not copied, on purpose

* Plans, credits, usage meters and upgrade banners: nothing here is sold.
* Telemetry and account-tied settings.
* Cursor's and ChatGPT's own system prompts and tool schemas: not read, and not wanted.

# How Cursor builds its tools around the agent

Read the same way (the installed copy's extension manifests, the message and tool names in its agent bundle, and the native helper's strings). Cursor's terminal, browser, file explorer and checkout code are **ordinary tools the agent calls through one typed interface**, not features the agent is bolted on to.

## Shape

* **The agent runs in its own process**, apart from the interface (an "AgentExec" extension host), with separate services for retrieval (indexing and search), a file service, MCP, and a local-inference runtime "outside workspace extension hosts". The explorer extension is 20 KB: the file tree is a view, and the agent's file tools are what act on files.
* **Every tool is a typed request with an enumerated result.** The vocabulary is small: list, read, write (edit), delete, grep, shell (streamed), background shell and write-to-its-stdin, diagnostics (from the language server), fetch, MCP, request-context, computer use, record screen. Each result is one of a fixed set, such as `success`, `file_not_found`, `read_permission_denied`, `write_permission_denied`, **`rejected`** (the person declined), `sandbox_unsupported`, `error`. Declining is a normal result the interface shows, not an exception.
* **The browser is an MCP server** that the agent uses like any other tool (driven through the browser's debugging protocol): `snapshot` (the page as an accessibility tree, which is how the agent reads it), navigate, click, fill, fill form, type, hover, drag, scroll, select, press key, tabs, screenshot, bounding box, **highlight** (show the person what is about to be touched) and **lock** (so the person's own clicks do not collide with the agent's).
* **Policy is layered and visible.** A sandboxing mode; **network** as an allowlist of domains with a separate "full network" request; whether git is sandboxed; a **read boundary** with an allowlist of extra paths the person may allow; a **command classifier** that returns each command's parts, a suggested sandbox mode and a **suggested allowlist entry** ("always allow this"); a command blocklist that always asks; per-MCP-server and per-tool allowlists; a browser allowlist; and a "smart" approval mode that decides routine requests and asks only about the rest.
* **A hidden "shadow workspace" and worktrees** let an agent get diagnostics or work in parallel without touching the person's files.

## The native sandbox helper (`cursorsandbox`, a static Rust binary, read with `strings`)

* Two backends chosen automatically: **Landlock** (filesystem rules, no privileges needed) and **bubblewrap** with user namespaces, with a **preflight** that exits 2 when the computer cannot sandbox, so the interface can say so before running anything.
* **seccomp** filters: one blocks the network, one blocks "dangerous syscalls". Capabilities are dropped; the process dies with its parent.
* The policy is one JSON file: workspace **read-write** or **read-only**, plus an optional network policy.
* **Config folders are blackholed**: an empty read-only directory is mounted over `.vscode`, `.idea`, `.cursorrules`, `worktrees`, `skills` and `agents`, so a command cannot rewrite the agent's own instructions.
* **Network goes through a local proxy** with a domain deny list and allow list, reached by a forwarder inside the sandbox; direct sockets are blocked.

## Where Helix stands

Helix already has the pieces Cursor lacks: the **Agent Surface** (typed actions with tiers, grants and undo), a **compositor-enforced approval dialog** that a client cannot imitate (`trusted-surface.md`), and desktop control behind a confirmer. Mobius's Tools-mode terminal (`terminal.rs`) is stricter than Cursor's default: bubblewrap with `--unshare-all` (no network at all), all capabilities dropped, system folders read-only, the project mounted at `/workspace`, a cleared environment, no stdin.

What Cursor's design suggests adding, in order of value:

| # | Idea | Size |
|---|---|---|
| 1 | **Typed tool results with `rejected` and `sandbox_unsupported`** across Mobius's tools, so declining and "cannot confine here" are results the thread shows, and a preflight that says whether sandboxing works before offering Tools mode | S |
| 2 | **Protect the project's own control files**: bind read-only (or blackhole) `.git/hooks`, `.git/config`, `.helix*` and the like inside the sandbox, so an approved command cannot plant a hook that runs later outside it | S |
| 3 | **Read-only mode** for commands that only look (`ls`, `grep`, `git log`), chosen by a small classifier, writable only when needed | S |
| 4 | **"Always allow this command"**: the classifier splits a command into parts and proposes an allowlist entry (`git status`, `cargo test`), stored per project and shown in Settings, so routine commands stop asking while the rest still do | M |
| 5 | **A seccomp filter** (a deny list of syscalls such as `ptrace`, `keyctl`, `io_uring`, `bpf`) added to the bubblewrap run | M |
| 6 | **Network by allowlist** instead of none: a local proxy that permits named domains (a package registry, for one approved install) and logs each request, shown in the thread | L |
| 7 | **The browser stage as a tool**: page text and an accessibility-tree snapshot as the agent's view, **highlight before acting**, and a **lock** while it acts; page text stays untrusted data | L |
| 8 | **Background commands** that keep running with their output followed, and a way to send them input | M |
| 9 | **A scratch copy or a git worktree** for an agent's edits, shown as a reviewed change, so a long task cannot half-edit the real project | M |

Not copied: Cursor's cloud agents, team policy and referral machinery, and its own system prompts and tool schemas (not read).
