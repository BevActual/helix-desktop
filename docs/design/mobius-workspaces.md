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
* `code_tree` and `code_read` in `main.rs`, `codestage.tsx`, `tools/nested/mobius-code-test.sh` (open Code, list files, view one, the chip, a turn's edit shows as marked with its diff, revert from the stage).

Still to do for Code: a real folder tree (it is a flat list with the folder in grey), editing in the viewer through the same reviewed `edit_file` path, the terminal and test output on the stage, and search across files.

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
* **Edits are descriptions, not code.** A fixed set (`Op`): crop, resize (percent or size), rotate (90, 180, 270), flip, brightness, contrast, black and white, invert, blur, sharpen, rectangle. Each is checked against the picture (nothing outside it, nothing out of range, up to 8,192 pixels a side, unknown fields refused) and applied by deterministic code with the `image` crate to a **copy**: every step is a new stored picture, the original is never written, and Undo, Redo and clicking a step in the history are only a position in the list (up to 30 steps per picture, 20 pictures per chat).
* **Drag on the picture** to select an area, then "Crop to selection" or "Draw it" (a rectangle in the chosen colour); buttons and sliders for the rest; **Save as…** writes PNG, JPEG or WebP with the system dialog, never over an existing file.
* **The model proposes in the same language.** A reply's fenced `imageops` block (a JSON list of up to ten of those descriptions) is drawn as a card, "Suggested picture edits", and **nothing is applied until the person presses Apply** on it, which applies them to the picture on the stage as steps they can undo. "Ask Mobius about this" sends a copy of the picture as it stands to the vision model with the next message.
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
