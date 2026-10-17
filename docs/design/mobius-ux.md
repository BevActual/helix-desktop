# Mobius: interface review

Status: 2026-10-17. A look at the interface as a person meets it, with what was changed and what is left. The pictures come from
`tools/nested/mobius-look-test.sh` (start screen, a typed message, a reply, a longer conversation, a narrow window), run in the nested
compositor with the scripted inference backend; run it again after any change to the layout and look at the result.

The lens is `helix-hig.md`: calm, quiet chrome, one obvious thing to do, nothing that needs explaining.

## Found, and changed

| Finding | Change |
|---|---|
| **The gap between turns was about 55 px**, twice what the layout asks for. The row of message actions (copy, edit, regenerate) is invisible until hover, but it held its space under every message. A conversation read as separate islands, and fewer turns fit on screen. | The actions sit in the gap below their message (absolute position) instead of holding space. Turns are 28 px apart. They still appear on hover and on keyboard focus. |
| **The start screen scrolled before the first useful thing.** Eight task cards of three or four lines each pushed "Or start a conversation" and the hint below the fold at the usual window size, and the composer showed a clipped two-line placeholder. | The cards are compact (smaller icon and padding, centred, a two-line limit) with shorter copy, so all eight fit above the composer. |
| **The composer's placeholder was a sentence** ("Ask anything — type / for commands (try /tools for research in chat)") that wrapped to two lines at the usual width, making the composer taller and clipping the second line on the start screen. | "Message Mobius — / for commands". The `/tools` hint is on the start screen already. With Tools on: "Ask a task — each step is shown for your approval". |
| **The row of chat settings** (persona, style, model, location, Tools, Context) was 11 px in the faintest label colour, so it read as a caption, not as controls. | Readable size and colour, with a visible hover state. |

## Added after looking at Claude, ChatGPT and Cursor

What those apps do that Mobius did not, kept to what fits a local, private assistant (Mobius already has the date-grouped list, pins, projects, search, Find, edit, regenerate, fork, a queued follow-up, drag and drop, paste and the shortcuts they are known for):

* **Chats are named by the model.** Claude and ChatGPT title a chat from its first exchange; the list was made of truncated first messages. After the first reply, the model that answered is asked for a two-to-six-word title as a background request (it never delays the next message). Only when that model runs on this computer, so no text goes anywhere it did not already go; only for a chat still carrying the title made from its first message (a rename wins, even one made while the model works); once. With the real models: "Cron vs Systemd Timer", "Fedora Wifi Drops Solution", "Good Morning Japanese Translation" (7B), similar from the 1.5B. `tools/nested/mobius-title-test.sh`; `HELIX_MOBIUS_NO_AUTO_TITLE=1` turns it off.
* **A reply being written no longer shows raw stars.** `**scheduler` showed until the closing pair arrived; open emphasis and code marks are closed for the display of a reply that is still streaming (a mark with nothing after it yet is held back).

* **Follow-ups queue as a list.** Typing while a reply is written used to keep one message and replace it with the next (a lost message). Each now waits in its own row above the composer ("Next · …", with Send now, which stops the reply and sends it next, and Remove), up to five, and they go one after another in the order typed. Escape clears them all. `tools/nested/mobius-queue-test.sh`.
* **A reply shows what it cost.** Beside its actions (on hover): the model, the tokens and the speed, e.g. `qwen2.5-7b-instruct-q4_k_m · 312 tokens · 41 tok/s`. Saved with the reply (`stats`), so it survives a restart; replies from before have none. What a person running a local model wants to know, and what Claude and ChatGPT do not have to show.
* **The Tools sheet is two rows and a note.** It was two paragraphs, five buttons and two notes that scrolled in a narrow window; now a switch, a folder row and one note, with the agent desk folded away.

## Models: tried in the real app, then changed

Looked at with the real service and this machine's own routing (`tools/nested/mobius-routing-look-test.sh`; the real-app run copies the routing file read only and never writes it back), against how Cursor does it: one model control in the composer, a list that applies on a click, the choice kept with the chat.

| Found | Changed |
|---|---|
| Switching a model was a modal: a backend dropdown, a list, a custom-id field, then **Use** (open, pick, confirm). The list showed file names where the library knew a friendly one (`Qwen2.5-VL-7B-Instruct-Q4_K_M.gguf`, a shard `…-00001-of-00002.gguf`). | **A menu above the chip.** Auto, then each installed model with its name, size, what it can do (sees pictures, code) and whether it is `ready now` or `loads on first use`; a check on the one in use; one click switches and closes. "Other backends and custom ids…" opens the old sheet; "Models & routing…" the routing page. |
| The chip beside the composer, the "Loading …" chip and the Chat route card on the routing page showed raw file names. | They use the model's own name (`Qwen2.5 VL 7B`). |
| **The choice was global.** Picking a model for one chat rewrote the `chat` profile in the routing file, so every chat, and the next launch, followed it. | **The choice belongs to the chat** (`route_backend`, `route_model` in the chat file, kept through a fork). Other chats keep following the default routing; "Use the default model" returns a chat to it; Tools mode uses the chat's model too. The routing file is no longer written from the composer. |

Measured with the real models, switching in one chat (the 5.5 GB vision model, then the 1.5B): the first reply 6 to 7 s (the model has to load), the reply after switching 1.6 s, and a new chat afterwards answered by the default model again. `tools/nested/mobius-routing-real-test.sh` runs that and reads which model wrote each reply from the saved details; it needs the two models installed and skips without them.

## Seen, not changed
* **The chat settings row is still cryptic.** "Helix" is the persona, "On this computer" is where the model runs. A single model control (icon, name, location) that opens one popover would
  say it better, with persona and style inside it.
* **Three attachment icons beside the composer** (file, picture, microphone) could be one "+" menu, the pattern people know; the composer would then have room for a longer line.
* **"Act" is an unexplained word** on a button next to Send. Its tooltip says what it does; the label could say it ("Do on desktop") or the control could become a mode of the composer.
* **The start screen is task-first, not conversation-first.** The composer is the most likely first action; the eight cards could fold into one row of four with "More" and the composer centred above them.
* **Light theme** has not been looked at since the apps began to follow the system theme (`settings-audit.md`). Take the same pictures with the light theme and read them.
* **Reordering the queue** (Cursor lets you drag queued messages).
* **Branches in place.** Claude and ChatGPT keep every version of an edited message with a `‹ 2 / 3 ›` switch; Mobius forks into a new chat (which is clearer about what is where, but moves you).
* **A side panel for what was made** (Claude's artifacts, ChatGPT's canvas): editable, versioned, exportable. Mobius has Save as file with a preview.
* **Regenerate with another model** (the menu changes the model for the next message; a reply cannot yet be redone on another one), and inline editing of a message instead of a sheet.
* **Choosing between models by what they are good at**: the routing page's task map is right for defaults, but the menu lists models flat; a first line ("Fast", "Sees pictures") per model would help someone who does not know the names.
* **A narrow window** (below 720 px) hides the chat list behind a button; the look script's narrow picture did not take effect (`set-window-width` did nothing in the nested compositor), so that layout was not reviewed here.

## Tests

The Mobius suites were run on `main` and failed the same way there, so these failures did not come from the changes above. Most were one cause, fixed in the suite: the nested window is 640 px wide, below the 720 px at which the chat list becomes a button and the sheets scroll, so tests that click the list or fill a sheet missed. They now run in a 1100 px window (the KDL for that has to be on several lines: one line with a nested block does not parse, and niri then starts with no spawned program). Now passing: `mobius`, `mobius-search`, `mobius-context`, `mobius-pictures`, `mobius-tools`, `mobius-lab`, `models`, and `mobius-dialogs` (its persona step looked for a rail that is hidden while the persona chip is on, and its model step for the old sheet; both updated, and the slash-menu check counts instead of assuming a number). Nothing in the Mobius suites fails now.


## Cursor, studied (2026-10-17)

From Cursor's documentation and reviews of Cursor 2.0 (I could not run Cursor here, so this is what it documents, not a hands-on session). What it does, and what Mobius should take from it, kept to what suits a local, private assistant.

| Cursor | Mobius today | Take |
|---|---|---|
| **Mode picker in the composer** (Agent, Ask, Plan, Debug), switched with Shift+Tab | "Tools" is a sheet; "Act" is an unexplained button beside Send | **One mode control** in the composer: Ask (chat only), Tools (research with approval), Act (desktop), Plan (a reviewable plan before anything runs). Shift+Tab cycles. Replaces the Act button and the Tools sheet's switch. |
| **One model control** and Auto with a stated policy (Cost, Balance, Intelligence) | A model menu with Auto as a row | Done in part. Add what Auto picked on the reply ("Auto → Qwen2.5 1.5B"), and the decision lane ([decision-model.md](decision-model.md)) behind Auto. |
| **`@` mentions**: files, folders, terminals, past chats, git diffs, browser | `/` commands, drag and drop, Context chip | **`@` in the composer** with a picker: files in the agent desk, other chats, the open window's title, the clipboard. Chosen items are pills above the text, each removable, so what the model sees is visible. |
| **Context ring**: a small ring by the input; click for a breakdown (system, tools, rules, history) | A "Context" chip in the settings row | A ring that fills as the chat approaches the loaded model's window, with the breakdown on click, and a "summarise earlier turns" action at the limit. Matters more locally, where windows are 4 to 32k. |
| **Checkpoints**: before significant edits the agent saves a restore point; restore returns the state without removing the chat | Act has Undo for one action; the agent desk has no restore | A checkpoint per Tools or Act turn that changed anything, shown on the message ("Restore"), built on `act-undo.md` for desktop actions and a snapshot of the agent desk folder for files. |
| **Review of changes**: a diff while it runs, Stop (Cmd+Shift+Backspace), Accept or Reject per file, a Review pass | Tools mode shows steps for approval; saved files show a preview | A **Changes** panel for a turn: files touched, a diff for each, Accept or Reject each, Stop in the composer while it runs. |
| **Queue**: Enter queues, Cmd+Enter sends now, drag to reorder | Queue list with Send now and Remove | Done. Add Cmd+Enter and drag to reorder. |
| **Several agents** side by side, each with status, in a list | One chat at a time; the list shows titles | A running indicator in the chat list for a chat still writing or waiting for approval, so leaving a chat does not hide it. |
| **Side chats** (`/side`) for a question that must not touch the main thread | None | `/side`: a one-off question in a small panel that sees the chat but does not join it. |
| **Plan mode**: researches, asks clarifying questions, produces a plan you edit, then builds | None | Plan in the mode control, with an editable checklist before Tools or Act runs it. |
| **Many models, one click**; per-model pricing | Menu shows size and abilities | Add a one-line reason per model ("Fast", "Sees pictures", "Best for code") and, for local ones, memory and measured tokens per second from the reply stats. |

### Order

1. **Mode control** (replaces Act and the Tools switch; the biggest clarity win, no new engine work).
2. **`@` pills and the context ring** (shows what the model sees; the ring needs the loaded model's window from the inference service).
3. **Checkpoints and the Changes panel** (trust in Tools and Act; needs the agent-desk snapshot).
4. **Running indicators, `/side`, Plan**.
5. **Auto with the decision lane**, which needs the `Embed` call.

Not taken: cloud-first model pricing and usage pools; many cloud agents in worktrees (Mobius is one person's assistant on one computer); anything that sends text off the computer by default.
