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
