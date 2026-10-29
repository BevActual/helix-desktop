# Actions: one catalogue for people and machines

The first pillar of `north-star.md`. An **action** is something Helix can do, described once: the person's buttons and an agent's tool
calls are the same actions, checked by the same policy, recorded in the same history and undone the same way.

## What an action is (`core/helix-actions`)

| Field | Meaning |
|---|---|
| `id` | `provider.verb`, e.g. `settings.set`. Shell commands keep their hyphenated names, so a dot means "catalogue action". |
| `title`, `summary` | For people, and for a model choosing a tool. |
| `params` | JSON Schema of the arguments. |
| `risk` | `observe`, `observe-private`, `act`, `confirm`, `blocked`: the Agent Surface tiers, so grants mean the same thing everywhere. A provider may make one call riskier (one setting), never less. |
| `undo` | `restore` (the value before is kept and put back) or `none`. |

## How a call runs (`org.helix.Desktop.AgentSurface1`)

`Action(id, {params: "<json>", reason})` for a catalogue id. The desktop service works out the tier of this exact call, then the existing
policy decides: blocked is refused, a missing grant is refused, `confirm` shows the person the change **in plain words** ("Lock screen:
On → Off", not JSON) in the trusted confirmation dialog. The audit line is written as for any command. The call runs in the service; the
result comes back in `Completed`. `ListTools` includes the catalogue (name, tier, title, summary, params); `ListActions` returns it as JSON.

## The history and Undo (`org.helix.Desktop.Configuration1`)

Every change lands in `$XDG_STATE_HOME/helix/actions/history.json`: when, which action, what it acted on, a sentence, **who** (`person`
"You", or `agent` with the client's label), before and after. The person's own changes from Settings are recorded the same way.
`ActionHistory(limit)` returns it newest first; `UndoAction(entry)` puts the value back (and records that as a change too).

* Only the newest change of a thing can be undone; undoing an older one would silently throw away what came after.
* Quick repeats by the same actor (dragging a slider) are one entry whose "before" is where it started.

## Providers

**Settings (v1).** Every row of the Settings catalogue (`GetSettings`), reached through three actions rather than one tool per setting, the
way App Intents use entity queries:

* `settings.find {query, page?}` (observe): matches by title, group, page and id, with each match's accepted values and how risky a change is.
  Private values (location) are left out of results.
* `settings.get {setting}` (observe; `observe-private` for location).
* `settings.set {setting, value}` (act by default, undo: restore). Checked against the values the setting accepts before the store sees it.
  * **Blocked** for agents: anything that runs a program (`hooks.*`, any `*command*` setting), key storage (`storage.*`), allowing an empty
    lock password.
  * **Confirm**: lock screen, idle, location, AI routing, Tailscale, the account picture.

**Next:** Models (slots, downloads, unload), Software (install, remove, update: confirm), the shell's commands as catalogue entries, then
Mobius's environments (Write, Image and Code operations already have per-stage operation lists to promote).

## Who uses it

* The `helix` command line (`core/helix-cli`; open by design: everything clickable is scriptable):
  `helix actions`, `helix find night light`, `helix get theme.mode`, `helix set theme.mode dark` (says what changed and how to undo it),
  `helix do <action> key=value …`, `helix history [-n N]`, `helix undo <entry>`, and `--json` on all of them. Changes go through Agent
  Surface as "helix command line" (the person allows it once, so an agent with a shell cannot quietly borrow it); history and undo are the
  person's own Configuration1 calls. A refusal says why in plain words. Nested: `tools/nested/ui-suite.sh cli`.

* MCP clients through `tools/helix-agent`: `settings_find`, `settings_get`, `settings_set` (allowlist entries of the same names). The tool asks
  once for a stricter grant when the service needs one.
* Mobius's tool loop: `settings_find` and `settings_set` in Tools mode and in tasks (`apps/helix-mobius/src-tauri/src/agent.rs`). Reading is
  free; a change asks the person in Mobius unless the task's policy always allows that setting (`allow_settings` in `helix-task`), and the
  desktop's own rules apply on top. Nested: `tools/nested/ui-suite.sh mobius-actions`.
* The bar: an agent's change appears in the island in plain words with Undo (for eight seconds; never over something that needs the
  person), and the screen edges are lit by the compositor while an agent acts (`HelixSetGlow`).
* The presence (next): asking anywhere runs actions; its results offer Undo from the history.
