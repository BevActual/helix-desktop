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

## Seen, not changed

* **Streaming shows raw markdown for a moment**: `**scheduler` appears until the closing `**` arrives. Hide an unclosed emphasis marker while a reply is streaming.
* **The chat settings row is still cryptic.** "Helix" is the persona, "On this computer" is where the model runs. A single model control (icon, name, location) that opens one popover would
  say it better, with persona and style inside it.
* **Three attachment icons beside the composer** (file, picture, microphone) could be one "+" menu, the pattern people know; the composer would then have room for a longer line.
* **"Act" is an unexplained word** on a button next to Send. Its tooltip says what it does; the label could say it ("Do on desktop") or the control could become a mode of the composer.
* **The start screen is task-first, not conversation-first.** The composer is the most likely first action; the eight cards could fold into one row of four with "More" and the composer centred above them.
* **Light theme** has not been looked at since the apps began to follow the system theme (`settings-audit.md`). Take the same pictures with the light theme and read them.
* **A narrow window** (below 720 px) hides the chat list behind a button; the look script's narrow picture did not take effect (`set-window-width` did nothing in the nested compositor), so that layout was not reviewed here.

## Tests

The Mobius nested suites were run on `main` and on this change with the same result, so none of the failures below comes from it. Several fail on `main`:

* `mobius-search` and `mobius-context` failed because the nested window is 640 px wide, below the 720 px at which the chat list becomes a button; the tests click the list. They now run in a 1100 px window and pass.
* `mobius` (chat list actions: search with no match, delete, new chat), `mobius-pictures` (opening a saved chat: `chat-0`, `edit-0`) and `mobius-tools` (`meta-tools`) still fail; at 1100 px they fail on other controls (`input`, `chat-0`, `meta-tools`), so they need their own look.
