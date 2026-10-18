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
