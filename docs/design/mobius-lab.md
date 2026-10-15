# Mobius lab: things being tried

Mobius is where we explore how an assistant should feel. This page lists what can be switched on and off to compare, where each piece lives,
and how to add the next experiment. Research notes (ChatGPT GPTs and Projects, Claude Styles, Gemini Gems, Poe, Raycast presets, Grok
Companions and Custom Agents, Hermes Agent) are the source of the ideas; what we kept is decided by trying it.

## The prompt, in layers

One function composes what the model is told, so each layer is visible, testable and removable:

```
identity            the persona's system prompt (or the default Mobius prompt)
Instructions for this project:   the project's instructions        [experiment: project_instructions]
Style for this conversation:     the chat's style overlay          [experiment: style_overlays]
```

`helix_shell_core::mobius_lab::compose_system(identity, project, style)`; empty layers add nothing. It is called from one place in the
host (`active_prompt`), so send, edit and regenerate always agree. Identity text is cleaned on save (invisible and direction-changing
characters removed, length bounded) and scanned for override phrases, shown as a warning beside the editor.

## Experiments

Stored in `~/.local/state/helix/mobius/lab.toml` (Settings in the app: **Models & routing → Advanced → Experiments**). All default on; turn
one off to get the earlier behaviour back.

| Key | What it adds | Off means |
|---|---|---|
| `persona_chip` | A persona chip in the composer with a menu (switch, edit, new) and `/persona`; the rail under the title is hidden. | The rail of persona chips under the title. |
| `style_overlays` | A style chip and `/style`: Concise, Plain language, Teacher, Technical, Creative, Formal, or your own. Remembered per chat. | No style layer; chip and command hidden. |
| `project_instructions` | A field in the project sheet; applied to every chat in the project. | Projects only group chats. |
| `persona_starters` | An empty chat shows the persona's own starter prompts. | The generic six. |

Persona fields added for this: `icon` (one of 14 symbols), `description`, `starters` (up to 4).

## Adding your own style

```toml
[styles.pirate]
label = "Pirate"
prompt = "Answer in the voice of a cheerful pirate, but keep the facts exact."
```

It appears in the style menu next to the built-ins. Project instructions are stored in the same file under `[projects]`.

## Adding an experiment

1. A boolean in `Experiments` (`core/helix-chat/src/mobius_lab.rs`), default `true`, with a test.
2. A case in `lab_set_experiment` (`apps/helix-mobius/src-tauri/src/main.rs`).
3. The page reads it from `lab_get` (`lab` in `chat.js` / the React dialogs) and shows the old or new behaviour.
4. A row in **Experiments** (`web/src/models.tsx`) and a line in the table above.
5. A step in `tools/nested/mobius-lab-test.sh`.

## Tests

- `cargo test -p helix-chat` covers composition order, styles, project instructions, scanning and persona cleaning.
- `tools/nested/ui-suite.sh mobius-lab` drives the chips, menus, sheets and the Experiments switches in the nested compositor and reads
  the composed system prompt from the program log (`HELIX_MOBIUS_LOG_PROMPT=1`, test only).

## Not tried yet (ideas from the research)

- `@persona` mention in the composer to switch for one message.
- Routines: the assistant proposes a saved sequence after a multi-step Act and you approve it (Grok Bot, Hermes skills).
- A memory layer: opt-in notes the assistant keeps across chats, searchable (Hermes).
- Persona files as markdown with front matter so they can be shared and diffed.
- A "Switched to Code" divider when the persona changes mid-chat.
