# Mobius: capabilities around everyday tasks

Priorities are based on current gaps in the code and everyday workflows. Each delivery should give someone a complete task they can start, inspect, stop, save, and resume.

## Current usable paths

| Task | Current path | Next useful improvement |
|---|---|---|
| Ask, explain, write, or work through code | Chat, persona/model selection, streamed answers, Stop, edit, regenerate, fork; **Tools mode** (`/tools`, welcome card, meta chip) for inline research and reviewed project commands | Deeper project-knowledge reuse across chats |
| Continue a long task | Saved chats, projects, full-text search, Find, and a per-chat brief | While you type (Ask, local), **From your files** chips preview what knowledge would attach; replies still list passages under **From your files · n** |
| Understand text files | Bounded attachments, excerpt cards, exact text preview | Extract readable PDF/document content and show page/source references |
| Understand a picture | Local vision model and attached pictures | A deliberate screenshot-to-question flow |
| Make a picture | Local SD-Turbo generation, size/seed, progress, Stop, save/copy/reuse | Reference-image editing and measured quality improvements |
| Dictate a prompt | Local microphone recording and Whisper transcription | Read-aloud replies, with playback and cancellation |
| Inspect a compiled program | Local REA/Ghidra evidence jobs and explanation | Better evidence navigation and comparison between binaries |
| Change the desktop | Act hands the request to the desktop assistant; a card above the composer shows it working or waiting for approval (Review, Stop); when it ends, the outcome (done, failed with the reason, or stopped) is written into the conversation and saved with it | Show the resulting state, not only the shell's words, and offer undo where an action can be undone (`act-undo.md`) |
| Reuse an answer | Copy text/code/table, export Markdown, **Save as file** on replies and on individual code blocks (preview + zenity save) | Table export shortcuts; richer export formats |

## Delivered: desktop tasks in the conversation

A request handed to the desktop assistant (Act) is a *desktop task*. While it runs, the card above the composer shows it. When it ends,
Mobius writes one note into the chat it was asked in: the request, and `Done`, `That didn’t work` with the reason, or `Stopped`. The
note stays where it happened (after the messages the chat had then), is saved in the chat file under `tasks`, appears in the Markdown
export, and is carried into a fork only if it happened before the fork's cut.

A task is deliberately not a message. It has no index, so edit, regenerate and fork work as before, and it is never sent to a model: the
result is text from the desktop, and a model should neither reason over it nor be steered by it. A chat that has only tasks is saved
and takes its title from the first request. Notes are bounded (100 per chat, the oldest dropped) and cleaned like any saved text.

When the shell recorded a pair, the note keeps those two display values, and Undo is shown while the shell still says that task can be undone. The button calls `Undo` on the shell. The pair is not sent to a model. A confirm-tier restore is classified in `act-undo.md` and not built, because the shell does not run those commands yet.

## Delivered: context and a chat brief

The Context chip and `/context` expose the recent history selected by the same helper that builds the model request. A ring on the chip and in the panel fills against the model window when `GetSnapshot.contextTokens` (local) or the OpenRouter catalog (cloud route) is known, otherwise against the recent-history character budget. The panel counts included/omitted messages and attached pictures, reports shortened message text, and states that adding a new prompt can change the selection. A boundary in the thread indicates where currently included history starts; older messages remain readable and saved.

A user-maintained brief (at most 2000 Unicode characters) carries goals, decisions, and constraints separately from the recent-history allowance. It is appended to the composed system prompt for text chat, including a persona's custom prompt. It persists with the chat, is inherited by forks, and is included in Markdown exports of a conversation. Old saved chats load with an empty brief. A brief-only draft appears in the sidebar before its first message. Temporary briefs stay in memory. Brief edits are refused during an active reply and if the selected chat changed; a failed save restores the previous brief and reports the error.

## Next delivery priorities

1. **Clear starting points — delivered.** The empty-chat screen opens the existing file, picture, image generation, dictation, software analysis, and chat-brief flows, with a model-library entry. Setup requirements remain visible in the actual flows. Starters fill a draft for review and preserve existing text. Dictation shows recording/preparing/transcribing state and prevents duplicate starts.
2. **Research with sources — delivered (Tools mode).** Per-chat Tools mode runs search, page read, and optional bubblewrap project commands inside the conversation supervisor. Steps appear inline with Stop; page reads and commands need explicit review; attachments or prior command output tighten review. Source links ride with the final answer. Network and shell stay in Mobius; `helix-inference` does not gain network access. The separate **Look online** sheet remains for hand-picked sources without Tools mode.
3. **Useful outputs (started).** Assistant replies offer **Save as file**: editable preview, suggested name, zenity save (create-new only). Reply-level save: one fenced block → language-tipped snippet; otherwise full reply as Markdown. Each code block in the thread also has its own **Save as file** shortcut into the same preview flow. Table replies also offer **Save as CSV**. Next: multi-artifact picks.
4. **Project knowledge.** Allow explicit project notes and selected files to be reused across chats. Show which sources reached the model and allow removal. Measure retrieval before adopting embeddings or presenting inferred facts as remembered decisions.
5. **Voice and task feedback.** Read-aloud replies with playback controls; Act progress and result evidence; consistent busy, retry, and error states across tools.

## How to judge a delivery

A workflow should make its input, selected model/location, included evidence, progress, result, and recovery action understandable. Test the real UI with bounded fixtures, verify persistence and cancellation, and measure model-dependent behavior separately from interface behavior. Do not mark a planned tool as available simply because its menu exists.


## Delivered: a task-oriented start screen

The React welcome screen exposes six task cards and a model-library entry, all wired to existing controls rather than new model-side tools. Images and analysis open their full dialogs; files and pictures use the native picker; voice starts/finishes local dictation; the brief opens Chat context. No model download or generation starts from merely opening a dialog. Persona-specific starters remain available when enabled. Generic starters are focused on planning, writing, and explanation. Choosing a starter fills the composer without submitting, and leaves an existing draft intact with an explanation. The hand-written welcome fallback uses the same draft-review behavior.

A separate voice busy state disables repeated starts and tracks preparation/transcription. The task card announces recording and offers Finish dictation; transcription is still inserted into the draft for review.

## Delivered: Tools mode (inline research and project commands)

Enable **Tools** from the chat meta chip. Settings persist per chat (`tools_enabled`, `workspace`) and survive fork. Software-analysis chats cannot enable Tools.

When enabled, each send runs a bounded tool loop before the talk model answers: the planner may request public web search, read a page, run a shell command in the chosen project folder, or finish. Activities are stored on the assistant message (audit + resume context). Regenerate and edit are refused while tool history is present; send a new message to retry.

Review rules: terminal commands always wait for **Run command**; page reads wait when the URL was not already in the user message; searches after attachments or any terminal step wait for **Allow lookup**. Declined steps are recorded and passed back to the planner. Stop cancels the loop, sandboxed commands, and stale approvals.

Project commands use bubblewrap: read-only system bind, writable project bind as `/workspace`, no network, no home or session paths. Desktop change still requires Act.
