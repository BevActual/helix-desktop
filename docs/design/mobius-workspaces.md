# Mobius workspaces: a surface that changes shape with the work

Status: design and first slice, 2026-10-17. The direction: Mobius is not a chat window with extras, it is a surface that rearranges itself for
the kind of work being done (writing, code, photos, vector design, browsing) and brings the right tools, context and layout with it. Chat stays
the way to ask; the *stage* beside it is where the work lives.

## The shape

```
┌ chats ┬───────────── chat ────────────┬────────────── stage ───────────────┐
│       │ messages, composer, modes     │ the work: a document, a repository, │
│       │                               │ an image, a vector drawing, a page  │
└───────┴───────────────────────────────┴─────────────────────────────────────┘
```

* **Workspace** = a layout (how wide the chat and the stage are, what side lists show: files, layers, outline, history), a **stage kind**, the
  **tools the model is given** for it, and **the context it is shown** (what is open, what is selected). Switching workspace changes all four at
  once; the chat and its history stay.
* **Chat** (today's Mobius, no stage), **Write** (a document stage: the person edits, the model drafts and revises, versions are kept),
  **Code** (files and a diff stage, the terminal and Changes card, project rules), **Image** (a canvas: crop, resize, adjust, remove background,
  annotate, with the model describing and proposing), **Design** (a vector canvas: shapes, text, layers, export SVG; the model edits through
  structured commands), **Browse** (a page next to the chat: the model reads what is shown and answers about it, acts only with approval).
* **Switching**: a switcher in the header and the command palette ("Workspace: Write"); Mobius may *suggest* one (a message that reads like
  writing a document gets "Open Write?", decided with the embedder in `decision-model.md`), never switch silently.
* The stage is a **panel**, not a window: resizable, collapsible, remembered per chat; below 900 px wide it takes the whole width with a way back
  to the chat.

## How the model works on the stage

The model never gets free access to a surface. It writes **structured, bounded operations** that Mobius applies and the person can see and
undo (the same rule as `edit_file` and charts):

| Stage | The model's side | Applied by | Safety |
|---|---|---|---|
| Document | a fenced `artifact:Title` block (the whole document or a revision) | the panel stores a new version | text only; versions never lost |
| Code | `edit_file`, `terminal` (exist) | review card, restore point, Changes card | reviewed before running |
| Image | `{"op":"crop","x":..}` and the like, a small fixed set | Rust image operations on a copy | original untouched; each op one undo step |
| Vector | `{"op":"add","shape":"rect",...}`, move, restyle, group | the editor's own model of shapes | no scripts; SVG is generated, never run |
| Browser | the page's text and structure (untrusted evidence); `click`, `type`, `navigate` as reviewed steps | a sandboxed webview in its own process | page text is data, never instructions; actions wait for approval |

What the model sees of the stage is explicit and visible: a chip above the composer ("Plan, document · 1,240 characters") that can be removed,
the same attachment mechanism `@` uses.

## Core workspace backlog (2026-10-20)

| Item | Status |
|---|---|
| Write toolbar → `stage_apply` | Done (`workpanel.tsx`, `mobius-write-test.sh`) |
| Changes per-hunk accept/reject | Done (`tools_accept_hunk`, `mobius-edit-test.sh`) |
| Context ring | Done for local + cloud: ring uses recent-history budget until `GetSnapshot.contextTokens` or the OpenRouter catalog reports a model window (~4 chars/token) |
| Knowledge citations | Done for v1: reply `From your files · n`, composer preview chips (`knowledge_peek`, respects Settings), `mobius-knowledge-test.sh` for attach-at-send |
| Region image ops | Done for v1: `blur_region`, `brightness_region`, `contrast_region` + contextual bar; `mobius-image-test.sh` exercises region blur |
| Parallel agent tasks | **Deferred:** needs a second inference slot and project-folder conflict rules (`mobius-workspaces.md` Code tasks) |

## Order of work

1. **The stage and the Document stage** (first slice, below): panel, layout presets, `artifact` blocks, versions, the open document goes to the
   model as a visible chip.
2. **Code stage**: file tree from the project folder, a viewer and the diff of a turn on the stage instead of in the thread.
3. **Image stage**: a canvas with a fixed set of operations in Rust; the vision model describes.
4. **Vector stage**: a shape model, a small editor (select, move, resize, text, colour, layers), SVG export, structured commands for the model.
5. **Browser stage**: a sandboxed webview, reading first, then reviewed actions.
6. **Suggestions**: the embedder proposes a workspace from what is being typed.

## First slice (built)

A work panel on the right of the chat. A reply's fenced block labelled `artifact:Title` shows as a card in the thread with **Open**; opening puts
the document on the stage, where the person edits it (it saves as they type, as a new version when the model's changes arrive or on demand),
sees earlier versions, copies it, or saves it as a file. While a document is open it is attached to what the person sends, as a chip they can
remove, so "make the second paragraph shorter" works on the text in front of them. `artifacts.rs` keeps the versions (up to 20 per document,
per chat); `workpanel.tsx` is the panel; `tools/nested/mobius-stage-test.sh` runs it.

## Second slice (built): the workspace switcher and the Code stage

* **The switcher**: "Chat ▾" in the header (Write and Code, with Image, Design and Browse listed as planned), and "Workspace: …" in the palette. Switching closes the other stage, and a different chat starts in Chat again.
* **Write** opens the Document stage ("Untitled" if the chat has no document yet).
* **Code** shows the chat's project folder beside the conversation: a filterable file list (text files, bounded, no hidden or build folders), a viewer with line numbers (first 3,000 lines, same boundary as the edit tool), and the files the last turn changed marked with their kind. A changed file shows its **diff** with **Revert this change** instead of its text. The file in view goes to the model with the next message as the same visible chip a document uses. Without a project folder the stage offers to choose one.
* `code_tree`, `code_read` and `code_search` in `main.rs`, `codestage.tsx`, `tools/nested/mobius-code-test.sh` (open Code, list files, view one, the chip, a turn's edit shows as marked with its diff, revert from the stage).

The file list is a tree: folders come first and collapse, and opening a file opens the folders above it. A filter still lists matching files flat, with the folder in grey. **Edit** turns the viewer into a text box for the whole file (files longer than the 3,000-line view stay read-only). Nothing is written until **Review change** shows the diff and **Accept** saves it through the same restore point as `edit_file` (Undo on the stage puts it back; a file that changed underneath is left alone). Opening another file, or closing the stage from its own button, waits until that edit is accepted or cancelled. Two or more characters in the filter also search inside those files (`code_search`): up to three lines a file and forty in all, skipping hidden folders, links and credential files. A match opens the file and scrolls to the line. A **Terminal** tab beside Files and Tasks mirrors the latest command's live output from the chat (the same stream the step card uses). **Open** on a line in the Changes card jumps to that file on the Code stage. Still to do for Code: test output on the stage. On the Changes card, a modified file with multiple `@@` hunks shows **Accept this hunk** (keep only that group) and **Reject this hunk** (drop that group).

## Modes as places in the sidebar (built, after looking at ChatGPT, Claude and Muse)

ChatGPT puts Library, Projects, Plugins, Scheduled, Codex and Explore at the top of its sidebar; Claude puts Projects, Code, Artifacts, Scheduled and Dispatch there; Muse keeps reorderable tabs (Main chat, Feed, Ideas, Goals, Library) above a separate list of side chats. The shared idea is that **a mode is a place you go**, listed before the conversations, not a setting inside one. Mobius now has the same: **Chat, Write and Code** as rows at the top of the sidebar (the header menu stays for narrow windows). When a stage is open on a window under 1400 px the sidebar shrinks to a rail of the three icons, so the work gets the room and the way back to Chat is always one click away.

What the three apps have that Mobius should add next, as places of their own: **Scheduled** (timers that start a prompt or an Act task, listed and editable), **Projects** (a project's chats, files, instructions and memory on one page), **Artifacts** (every document and chart this chat or project made, openable on the stage), and **side chats** (a one-off question that sees the main chat without joining it; Muse lists them under the tabs, Cursor has `/side`). Muse also lets the person **reorder** the tabs and decide which exist; the same should hold here, with the later stages (Image, Design, Browse) appearing as places only once they work.

## Artifacts and side chats (built)

* **Artifacts**: a fourth row in the sidebar. It swaps the chat list for every document the chats have made (title, the chat it came from, words, versions), newest first; choosing one opens that chat and the document on the Write stage. `stage_all` in `artifacts.rs`.
* **Side chats** (`/side`, or "Side chat" in the palette): a temporary chat that has been given the main chat as an attachment to read, with a chip, "Side chat · reads "Plan" · back to it", that returns to the main chat. The main chat is untouched and the side chat is not saved, so a question that would clutter the thread has somewhere to go. It is built on the temporary chat and the attachment path rather than on a second conversation inside one, which keeps the main chat's history exactly what the person made it. `tools/nested/mobius-side-test.sh`.

## Scheduled (built)

A place in the sidebar for prompts that run by themselves (ChatGPT and Claude both list "Scheduled"). It is where a desktop assistant is stronger than a website, because the operating system already has a scheduler:

* **The timing is systemd's.** Each task is a user timer and a oneshot service, `~/.config/systemd/user/helix-mobius-task-<id>.{timer,service}`, that run `helix-mobius --run-scheduled <id>`. It fires whether or not Mobius is open, and `Persistent=true` catches up a run missed while the computer was off. Only the task's id is in the unit (never the prompt or the name beyond the description); times and dates are validated before a unit is written (`on_calendar`), and `systemd-analyze` accepts the calendars and units it writes.
* **The run is headless**: no window. It asks the model on this computer (with Memory), saves the answer as a new chat named after the task, records the run and tells the person with a desktop notification ("Morning brief is ready").
* **It only chats.** No tools, no desktop actions, and a cloud route is refused ("only a model on this computer"), so nothing unattended can change the computer or send text away. Tools and Act would need an approval nobody is there to give.
* The sheet: add (name, prompt, every day, certain days or once, a time), switch on or off, **Run now**, delete, the last run and a link to its answer, up to 12 tasks. `scheduled.rs` (unit tested), `scheduled.tsx`, `tools/nested/mobius-scheduled-test.sh`.

Not done: a real timer firing (the nested test writes the units and runs the same command without starting systemd), editing a task, a task that continues in an existing chat, and the shell's notification opening the answer.

## Projects (built)

A place in the sidebar (ChatGPT and Claude both list "Projects"). A project was a name on a chat plus optional instructions behind a dialog; now it is one page with what its chats share:

* **Instructions** every chat in the project follows (the existing project instructions, edited here), **a folder** the project's chats work in (new chats joining the project get it as their Tools and Code-stage folder; Tools mode itself stays as it was), the project's **chats** (open one) and the **documents** they made (open one on the Write stage).
* **New chat in this project** starts a chat that joins the project with its first message (an empty chat is not saved, so it cannot join earlier). Projects can be added on the page before any chat is in them.
* `projects_overview`, `project_create`, `project_set_folder` in `main.rs`; `Lab.project_folders` and `declared_projects` in `mobius_lab.rs` (unit tested); `projects.tsx`; `tools/nested/mobius-projects-test.sh` (add a project, instructions and folder saved, a new chat joins it and gets the folder, the page lists it).

Not done: moving a chat to a project from the page (the chat header still does it), a per-project memory, and project files indexed for questions (the embedder in `decision-model.md` is the piece for that).

## The Image stage (built, first set of operations)

* **A place** ("Image" in the sidebar, the workspace menu, the palette) and **Edit** on any picture in a chat. A picture opens beside the chat; "Open a picture…" uses the system picker. `imagestage.tsx`, `imagestage.rs`, `imageops.rs`.
* **Edits are descriptions, not code.** A fixed set (`Op`): crop, resize (percent or size), rotate (90, 180, 270), flip, brightness, contrast, black and white, invert, blur, sharpen, rectangle, and region-only blur/brightness/contrast (`blur_region`, `brightness_region`, `contrast_region`). Each is checked against the picture (nothing outside it, nothing out of range, up to 8,192 pixels a side, unknown fields refused) and applied by deterministic code with the `image` crate to a **copy**: every step is a new stored picture, the original is never written, and Undo, Redo and clicking a step in the history are only a position in the list (up to 30 steps per picture, 20 pictures per chat).
* **Drag on the picture** to select an area, then "Crop to selection" or "Draw it" (a rectangle in the chosen colour); buttons and sliders for the rest; **Save as…** writes PNG, JPEG or WebP with the system dialog, never over an existing file.
* **The model proposes in the same language.** A reply's fenced `imageops` block (a JSON list of up to ten of those descriptions) is drawn as a card, "Suggested picture edits", and **nothing is applied until the person accepts**: the card's *Preview on the picture* shows the result right on the canvas (`imgstage_preview` renders it without storing anything), with a checkbox per edit, and **Accept** (Ctrl+Enter) applies the chosen ones to the picture on the stage as steps they can undo, tagged Mobius; **Reject** (Esc) drops them. "Ask Mobius about this" sends a copy of the picture as it stands to the vision model with the next message.
* Tests: five unit tests for the operations (bounds, injection-shaped JSON, the original untouched, PNG/JPEG round trip); `tools/nested/mobius-image-test.sh`.

Not done: text annotation (needs a font), freehand and arrows, background removal (needs a model), layers, and a way to give the model the picture and its edit list so it can propose the next step from what it sees (the vision model can already be asked about the picture).

## Focus layout (built) and the more radical directions

**Focus layout.** With a stage open, a button in the header (and Ctrl+\) turns the window inside out: the stage fills it, the sidebar is a rail, and the chat becomes a small window floating over the work, an assistant in an editor rather than a document beside a conversation. The choice is remembered. This is the first layout that is not "chat with a panel": the work is the main thing and the conversation serves it. `tools/nested/mobius-stage-test.sh` checks the stage grows and the chat becomes a small window, and that it goes back.

Directions that follow from "Mobius reshapes itself for the task", roughly in the order they pay off:

1. **Ask on the selection, in the work.** Select text in a document and a small bar appears on it (Shorter, Rewrite, Explain, Ask…): the reply comes back as a visible change to that text, accepted or rejected in place, not as a message to copy across. The same in Code (select lines, "Edit with Mobius", the diff on the stage) and on the Image (select an area, "Describe" or a suggested edit for it). The chat dock then only needs to be a command line.
2. **A home that knows the task.** The empty-chat welcome (eight cards) becomes a surface of places to go: the workspaces with a preview of what is in each (the last document, the last picture, the folder's changed files), what is running or waiting for approval, scheduled results that arrived, and a single "What are you doing?" box that picks the workspace from the words (the embedder in `decision-model.md`).
3. **The dock follows the work.** In focus layout the chat dock can be dragged, collapsed to a single line (the last reply and the composer), summoned by typing anywhere on the stage, and fades while the person works; a reply that needs a decision (an approval, a suggested edit) raises it.
4. **Pinned live panels.** A chart, a table, a checklist or a `ui` block can be pinned to the stage as a panel that stays and can be re-run ("refresh this chart from that file"): ephemeral UI that becomes a small dashboard when the person wants it to.
5. **Voice mode** as a layout: a quiet full-window surface for talking with the model on this computer (dictation exists; read-aloud and a conversation loop come first).
6. **The conversation as a map**: forks, side chats and the documents each made, drawn as a tree the person can walk, instead of one long scroll.
7. **Summon from anywhere**: a key that raises Mobius over whatever window is focused and hands it that window's title and selection (the Agent Surface already knows the focused window), with the same approvals.

## Ask on the selection (built), and what other applications taught

Researched (documentation and reviews, not used hands-on): **Notion AI** (select text, "Ask AI": improve, fix grammar, shorter, longer, change tone, simplify; then replace or insert below), **Apple Writing Tools** (rewrite, proofread, summarize as key points, list or table, in any text field system-wide), **Cursor / Zed / GitHub Copilot** (Cmd/Ctrl+K or +I: a small prompt bar on the selection, the answer as a diff, Enter or Cmd+Y accepts, Esc rejects), **Photoshop's Contextual Task Bar** (a bar that follows the selection with the next sensible actions, can be pinned or hidden), **Canva** (a contextual toolbar for the selected object), **Figma** (Ctrl+K actions that depend on the selection), and the **AI browsers** Dia, Comet and Neon (the assistant reads the page or the tabs, and in the newest acts with permission).

What Mobius took:

* **A bar on the selection in a document**: select three or more characters and a bar offers Improve, Shorter, Longer, Simplify and Fix grammar, plus a free instruction (Ctrl+I, as in Copilot and Zed). The answer is a **proposed change shown old and new** that nothing writes until **Enter** accepts it; **Esc** rejects it. It is accepted as the person's own edit (a new version), and refused if the text changed while the model worked. It uses the model on this computer only (a selection is private text), and takes off the quotes, fences and thinking small models wrap around an answer. `inline.rs` (unit tested), `workpanel.tsx`, `tools/nested/mobius-inline-test.sh`.
* Found on the way: the document box could not select text at all (the window turns `user-select` off for everything and the textarea had not turned it back on).

**Code, too (built):** in the Code stage, click or drag over line numbers (shift-click extends) and a bar appears with Simplify, Add comments, Fix bug, Make it idiomatic and a free instruction. The model gets the file with the selected lines marked and about 40 lines of context each way, and returns the replacement for those lines only; the answer is a diff, **Enter** applies it (as a restore point the stage's **Undo** puts back, the same one `edit_file` uses), **Esc** rejects it. It refuses when the lines changed since the suggestion, and shares the file boundary of the edit tool (inside the folder, not hidden, not credentials). The same restore point covers a change the person types themselves: **Edit**, then **Review change**, then **Accept**. `inline.rs`, `codestage.tsx`, `tools/nested/mobius-code-test.sh`.

Next, from the same research: the same idea for the **Image** (the Contextual Task Bar idea: a selection shows the actions that fit it, Crop, Rectangle, Blur, "Describe this", beside it instead of in a toolbar far away), Apple's output shapes (summarize as key points, a list or a table), and the AI browsers' pattern for a **Browse stage**: the assistant reads the page beside it, and acts only with approval.

## Write: one mode taken to a high level

The Document stage is now a writing tool, not a text box. (Direction from the person: pick one mode and keep working it until it reads as Apple-grade: calm, exact, fast, keyboard-first, nothing that needs explaining.)

* **The page.** A centred column (46 em, 40 in focus mode), generous line height, a large title that is a real heading and renames the document, a system-serif/sans/mono choice and a size (Aa menu, remembered), the caret and selection in the accent colour, a status line (words, reading time, selected characters, "Mobius is writing…"). Chrome is a quiet toolbar, a segmented **Write | Read** control, an outline, history, and a **⋯** menu; panels fade in (reduced motion respected), and the history and the narrow outline slide *over* the page rather than squeezing it. Container queries decide when.
* **Read** is the same Markdown the thread draws, set as a document (titles, quotes, lists, tables, code), so what is written and what is read agree.
* **Formatting without leaving the keyboard.** Bold, italic, code, three heading levels, lists, numbers, quote, link from the toolbar or Ctrl+B, Ctrl+I, Ctrl+K; Enter continues a list (and an empty item ends it), Tab and Shift+Tab indent. Every change goes through the browser's own editing command, so **Undo and Redo keep working across toolbar edits and accepted suggestions**. The pure edit functions are unit tested (`web/tests/markdownEdit.test.ts`, `npm test`, 11 tests).
* **A block menu**: type `/` at the start of a line (headings, lists, quote, code block, divider, **Continue writing**, which proposes one or two paragraphs for the person to accept), shown at the caret.
* **Outline** of the headings (hidden by default when the stage is narrow), **find and replace** (Ctrl+F, replace one or all, "n of m"), a **document menu** (switch, **New document**), **History**: every version with who made it and when, the diff against the one before it, and **Restore** (which becomes a new version, so nothing is ever lost).
* **Ask on the selection** (previous section) lives on the same page.
* **Export**: Markdown file, a self-contained HTML page (escaped, no scripts, nothing fetched, light and dark), and **Print or save as PDF** (the Read view with a print stylesheet), plus **Focus mode** (the chrome fades away; the document is all there is).

Next for Write: smart punctuation, spelling suggestions through Mobius, a "make a table" block, and drag to reorder sections from the outline.

## One set of tools for people and machines, and a Bots mode (direction, 2026-10-18)

The direction from the person: each mode should grow into the kind of environment its work needs (Word for writing, Photoshop for pictures, an IDE for code), and **what a person can do there, a machine can do too**, through the same tools. Then **a Bots mode**, where several bots use those tools together to get a task done.

**Principle.** A stage owns one list of *named, typed operations* (the image `Op` set is the model: crop, resize, rotate...). The buttons a person presses, the suggestions the model makes, and the calls an agent sends are all that same list, checked by the same code, written to the same history, undone by the same Undo. There is no second, privileged path for machines and no hidden one for people.

* **Attribution.** Every history step says who made it. Built for the Image stage (`by`: you, or Mobius when the person applied its suggestion, shown as a tag in the history); Write versions already record who made them; Code edits come with the agent's name. Bots will each have their own name.
* **Same undo.** A bot's change is a step like any other, so one Undo reverses it; nothing a bot does is outside the history.
* **Reachable from outside.** The same operation lists can be offered to an external agent (through ACP today for files and terminals, and an MCP server for stage operations next), with the same approvals.

**Environments, per mode.** Write gains what a word processor has (styles, page view, comments and suggestions in the margin, tables, find across documents); Image gains a tool rail, an options bar, layers and a history panel (the Contextual Task Bar idea from the research above); Code gains a real file tree, tabs, search, a terminal and test output on the stage, and diagnostics. The layout is the mode's, not shared: panels dock where that kind of work expects them.

**Bots mode.** A place where the person gives a goal and a small team of bots works on it:

* A **bot** is a role (Researcher, Writer, Editor, Coder, Reviewer, Illustrator) with a model, instructions, and a **tool allowance**: which stages' operations it may use. Nothing is granted by default; a Writer cannot run a terminal because the allowance says so, not because the prompt asks.
* They work **in tandem on the shared stages**: one drafts a document, another proposes edits to it, a third makes the picture. Handing over is a visible message between bots, kept in the transcript.
* The person sees a **board**: each bot, what it is doing, what it changed (attributed steps), what waits for approval. **Stop all** is always one click; each bot has a step and time budget; anything that changes files outside the stages, runs a command or leaves the computer needs approval exactly as for a single agent. Bots use models on this computer unless the person allows a cloud route, and Act never uses one.
* It needs, in order: the shared operation registry per stage (Write and Code first, Image has it), attribution everywhere, a scheduler that gives each bot turns and enforces budgets, then the board.

### Write operations (built, first slice of the shared tools)

The Document stage now has its operation list, `docops.rs`: `replace` (one passage, or `all`), `delete`, `insert_after` (after the line holding an anchor), `replace_section` (everything under a heading up to the next one of the same or higher level; headings inside code fences do not count) and `append`. Each is checked against the text (the passage must exist and be the only one unless `all` is said; unknown operations and fields are refused; at most 10 edits of 20,000 characters), applied all or nothing, and recorded as a new version, so Undo is History. A reply's fenced `docops` block (a JSON list) is drawn as a card, "Suggested document edits", listing each edit in words, with **Review in the document**: the edits open as a pending change *on the page* (`stage_preview` computes the result without recording it): each edit has a checkbox, the diff shows what the ones left on would do, **Accept** (Ctrl+Enter) applies them as one new version and **Reject** (Esc) drops them. **Nothing changes until Accept.** The open document is sent to the model with a short note on this format, so small changes come back as edits instead of a whole rewrite. `stage_apply` takes the same list from any caller, which is what the toolbar and, later, outside agents will use. Tests: six unit tests (`cargo test docops`) and `tools/nested/mobius-docops-test.sh`.

Not done: margin comments and suggestions as operations, and attributing versions to a named bot. The Write toolbar, shortcuts, slash inserts, and find-and-replace go through `stage_apply` as `docops` replace/append operations. Typing debounces to `stage_record`. **Accepted selection suggestions** (Ask on the selection) record the full document as a **person** version via `stage_record` so repeated lines in the draft cannot block a unique `replace` (toolbar edits still prefer `stage_apply` when the ops are unambiguous).

## The chat shapes itself to the mode (research, 2026-10-18)

The direction from the person: the chat window should transform to fit the format of the mode, not stay a conversation column beside every stage. Researched from documentation and write-ups (not used hands-on): Zed's agent panel docs, the Cursor 2.0 Agents view and forum threads about it, Adobe's March 2026 announcement of the Photoshop and Firefly AI Assistant, and comparisons of Claude Artifacts, ChatGPT Canvas and Perplexity Spaces; plus the Cursor study already in `mobius-research-2026-10.md`.

**What the others do**

| Tool | What the assistant looks like | What to take |
|---|---|---|
| Cursor 2.0 | Two layouts: the editor (files left, chat on the side) and an **Agents view** where the *agents are the sidebar* (each with status, progress, a log) and the work is what they produced; parallel agents on one task, results compared | In Code the chat becomes a list of **tasks with state**, not one long thread |
| Zed | Agent panel with **threads** (several at once, a sidebar of them), **terminal threads** beside agent threads, a **Review Changes** view with per-hunk accept or reject, **Restore Checkpoint**, a **follow the agent** switch that jumps the editor to each file it touches, `@` mentions for files, symbols, diagnostics, selection | Review is on the work, per hunk; following is a switch; threads are cheap |
| Photoshop / Firefly | A chat assistant that either applies edits or **walks through them step by step**; a **Contextual Task Bar** that follows the selection; **AI Markup**: draw on the picture and write a prompt for *that* area; voice | In Image the prompt belongs **on the picture**, next to the selection; and a choice between "do it" and "show me how" |
| ChatGPT Canvas | Document beside chat; select text and ask; suggestions applied in one click; versions kept automatically; opens by itself when the task fits | Already matched (Write); the self-opening should stay a suggestion |
| Claude Artifacts | Document beside chat, mostly viewed (edited by the model); runs code to preview | The *preview* is part of the stage (run what was made) |
| Perplexity Spaces | A shared project space with uploaded sources; answers with citations | Projects and the knowledge store, not a stage |

**The idea they share.** The conversation is one *surface* of the assistant, not the assistant. In each tool the assistant has several: a prompt where the person is looking (the task bar, the selection), results as *pending changes on the work* (hunks, tracked suggestions, a preview) and not as text to copy, a status view for things running, and the full conversation behind a click for when talk is the point.

**Design for Mobius: a dock with four presentations.** The chat (the "dock") keeps one conversation and one history, but each workspace chooses how it is drawn:

1. **Conversation** (Chat): the thread as today.
2. **Margin** (Write): the thread collapses to a quiet column of cards anchored to the document: a suggestion beside the paragraph it concerns, accept or reject in place; the composer is a single line under the page ("Ask about this document", with Shorter, Continue, Outline as chips). The full thread opens from a button.
3. **Task bar** (Image): the prompt is a bar that follows the selection on the canvas ("Describe", "Remove", "Make it warmer" for this area); the model's suggested edits show as chips in the options bar with a ghost preview on hover, applied on click; a switch picks *Apply* or *Show me step by step*; the conversation is the History tab (each prompt next to the steps it made).
4. **Tasks** (Code, and later Bots): the thread becomes a list of tasks, each with a state (running, needs you, done), its step timeline (already built) and its changed files; a task opens to its full thread; several can run; a *follow* switch makes the Code stage open each file as the agent touches it; terminal output is a task of its own.

**How it is built without five chats.** A workspace declares, in one object: its **dock presentation**, the **composer** (placeholder, quick-action chips, what `@` offers), the **default context chips** (open document, selection, folder), the **shortcuts**, and which **reply blocks are stage-bound**. A stage-bound block (`artifact`, `docops`, `imageops`, `edit_file`) is not drawn in the dock as a card to press; it becomes a *pending change on the stage* (ghost text or hunk, ghost preview, diff) with accept and reject there, and the dock keeps a one-line summary ("3 edits proposed"). This is the same rule as the shared tools: the model's output and the person's are the same operations, so they appear in the same place, differing only in the attribution tag.

**Order of work**

1. The **workspace descriptor** and the dock presentations: composer text and quick-action chips per workspace (cheap, visible at once), then the Margin and the Task bar presentations.
2. **Stage-bound pending changes**: `docops` as a diff on the page with per-edit accept or reject; `imageops` as a ghost preview with Apply; both keep the dock summary.
3. **Tasks** for Code: the list, states and the follow switch (the timeline and permission cards exist).
4. **Voice** on the Task bar (dictation exists) and **Show me step by step** on Image.
5. The same descriptor is what a **bot** is given: its allowed stages, composer-less, with its output going to the stage and the board.

Open questions, to decide by trying them: whether the Margin should be anchored (needs the document to keep stable ranges for comments) or a simple column; whether the dock should ever hide completely in a stage (the focus layout already floats it); and how a single chat shows when two stages are open (Write and Image for a document with pictures).

### Panes the person can shape, and the first dock presentation (built)

From the person's note after looking at Cursor: the panes should be **resizable and movable**.

* **Resize.** A divider on the edge of the work area: drag it, or focus it and use the arrow keys (Shift for bigger steps), double-click or Home to go back to the default. The width is remembered **per workspace** (Write, Image and Code each keep their own), clamped between 300 px and about 78 % of the window. It is a separate `role="separator"` element with a label, so it works from the keyboard and with a screen reader.
* **Move, to any edge.** The ⇄ handle in the chat header: **drag** it toward an edge of the window and a translucent preview shows where the work area will sit (left, right, above or below the chat); or **click** it for a menu of the four places (keyboard reachable). The position is remembered; above and below keep their own height, remembered per workspace like the width, and the divider then resizes vertically (Up and Down arrows). Under 900 px wide the stage already takes the whole window, so these controls do not show there; the focus layout (chat floating over the work) is unchanged and hides them.
* **Starters per mode.** While a work mode's chat is empty, the general welcome cards give way to what fits: Write offers *Make it shorter, Fix spelling and grammar, Continue writing, Suggest an outline*; Image offers *Describe this picture, Brighter, Black and white, Crop to a square* (these carry the `imageops` format so a small model answers in edits the person can apply); Code offers *Explain this project, Find likely bugs, Add tests, Summarise recent changes*. The composer's wording follows the mode too ("Ask, or say what to change"). One descriptor object, `WORKSPACE_UI` in `chat.js`, holds this; the rest of the descriptor (dock presentation, default context, stage-bound blocks) is still to come.
* Tests: `tools/nested/mobius-panes-test.sh` (drag widens the stage, per-workspace memory, reset, the other side and back, the starters change with the mode and one sends its prompt).

Not done: splitting the work area itself (two stages side by side, for a document and the picture it uses), tearing a pane off into its own window, and a saved layout per project. Next: the Margin and Task bar presentations.

### A sidebar that folds, and tabs for the open views (built)

From the person's note after looking at Cursor's window: the conversation list can fold away, and each kind of work is a tab you can leave open.

* **Fold.** The sidebar button in the chat header (the same one that opens the drawer on a narrow window) folds the sidebar to an icon rail at 720 px and wider: the places stay, one icon each, and the chat list hides. It is remembered. Below 720 px it is still the drawer.
* **Tabs.** Chat, Write, Image and Code are tabs under the header once a second one is open. Opening a place adds its tab; closing a tab leaves Chat, which stays. The open set is remembered for the session. The sidebar rows and the workspace menu still switch the same places.

### Tools, from ArtCraft and Warp (2026-10-19)

ArtCraft's crafting apps (PhotoCraft, VectorCraft, WordCraft and the rest) make every control a named command, and the same command is what a person presses and what an agent may call. Warp's agent runs in a real terminal and waits before a command runs. Mobius already kept one operation list per stage. The Tools sheet now names that list for the place you are in (Write, Image or Code), and says that a terminal command always waits and can be stopped. The research tools stay the switch above it.

### Code: tasks and Follow (built)

The Code stage's side column has two tabs, **Files** and **Tasks**. Every request in the chat is a task (`codeTasks()` in `chat.js`, from the messages and their activities): newest first, with a state (**Running**, **Needs you** with a ringed row while the agent waits for an approval, **Failed**, **Done**), the request's first line, and "3 steps · 1 file changed". Clicking one scrolls the conversation to that request. A pulsing dot on the tab shows something is running. **Follow** (header, remembered, off by default as in Zed) opens in the viewer each file the agent touches while it works, taken from the newest step that names a file of the project. `tools/nested/mobius-tasks-test.sh` runs the fake agent and checks Needs you, Done and Follow.

Not done: tasks that run in parallel (one chat is still one turn at a time — the composer queue holds follow-ups, but a second agent turn needs a separate inference slot and conflict rules on the project folder; deferred until that contract is designed), a task opening to its own thread, and terminal output as a task of its own.

### Image: a prompt bar that follows the selection (built)

Drag a rectangle on the picture (Crop or Box tool) and a small bar appears beside it: **Describe**, **Read the text**, and a field, "Ask about this area…" (Enter sends, Esc clears the selection). The area is copied as a picture of its own (`imgstage_region`; the document is untouched) and goes to the vision model with the question as a normal message, so the answer lands in the chat. The idea is Photoshop's Contextual Task Bar and AI Markup: the prompt is where the person is looking. It asks *about* an area; edits still come from the toolbar or the model's previewed suggestions, because the edit operations apply to the whole picture except crop and box. Tests: `tools/nested/mobius-image-test.sh`.

**Region edits (2026-10-20):** `blur_region`, `brightness_region`, and `contrast_region` in `imageops.rs` change only the selected rectangle; the contextual bar offers **Blur area**, **Brighten area**, and **More contrast**, and models can suggest the same ops with `x`, `y`, `w`, `h`. Not done: every global adjust as a region op, or feathered masks.
