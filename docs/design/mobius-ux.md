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

## Seen, not changed
* **The chat settings row is still cryptic.** "Helix" is the persona, "On this computer" is where the model runs. A single model control (icon, name, location) that opens one popover would
  say it better, with persona and style inside it.
* **Three attachment icons beside the composer** (file, picture, microphone) could be one "+" menu, the pattern people know; the composer would then have room for a longer line.
* **"Act" is an unexplained word** on a button next to Send. Its tooltip says what it does; the label could say it ("Do on desktop") or the control could become a mode of the composer.
* **The start screen is task-first, not conversation-first.** The composer is the most likely first action; the eight cards could fold into one row of four with "More" and the composer centred above them.
* **Light theme** has not been looked at since the apps began to follow the system theme (`settings-audit.md`). Take the same pictures with the light theme and read them.
* **Several queued messages.** Cursor queues follow-ups in a visible, reorderable list and can send one now; Mobius holds one.
* **Branches in place.** Claude and ChatGPT keep every version of an edited message with a `‹ 2 / 3 ›` switch; Mobius forks into a new chat (which is clearer about what is where, but moves you).
* **Per-reply details.** The model, how many tokens and how fast: what a local-model user wants to know. `Completed` already carries the token counts; the message would need to remember them.
* **A side panel for what was made** (Claude's artifacts, ChatGPT's canvas): editable, versioned, exportable. Mobius has Save as file with a preview.
* **Regenerate with another model**, and inline editing of a message instead of a sheet.
* **A narrow window** (below 720 px) hides the chat list behind a button; the look script's narrow picture did not take effect (`set-window-width` did nothing in the nested compositor), so that layout was not reviewed here.

## Tests

`mobius`, `mobius-tools`, `mobius-search`, `mobius-pictures` and `mobius-context` were run on `main` and on this change and failed the same way, so none of the failures below comes from it; `mobius-act`, `mobius-welcome`, `mobius-reply-save` and `mobius-attachments` pass on this change. Several fail on `main`:

* `mobius-search` and `mobius-context` failed because the nested window is 640 px wide, below the 720 px at which the chat list becomes a button; the tests click the list. They now run in a 1100 px window and pass.
* `mobius` (chat list actions: search with no match, delete, new chat), `mobius-pictures` (opening a saved chat: `chat-0`, `edit-0`) and `mobius-tools` (`meta-tools`) still fail; at 1100 px they fail on other controls (`input`, `chat-0`, `meta-tools`), so they need their own look.
