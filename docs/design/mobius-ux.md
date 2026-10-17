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
* **Regenerate with another model**, and inline editing of a message instead of a sheet.
* **A narrow window** (below 720 px) hides the chat list behind a button; the look script's narrow picture did not take effect (`set-window-width` did nothing in the nested compositor), so that layout was not reviewed here.

## Tests

The Mobius suites were run on `main` and failed the same way there, so these failures did not come from the changes above. Most were one cause, fixed in the suite: the nested window is 640 px wide, below the 720 px at which the chat list becomes a button and the sheets scroll, so tests that click the list or fill a sheet missed. They now run in a 1100 px window (the KDL for that has to be on several lines: one line with a nested block does not parse, and niri then starts with no spawned program). Now passing: `mobius`, `mobius-search`, `mobius-context`, `mobius-pictures`, `mobius-tools`, `mobius-lab`, `models`. Still failing:

* `mobius-dialogs`: the persona rail (`persona-dot-0`, `persona-dot-add`) is not on the page it opens; the rename and project sheets in the same test pass.
