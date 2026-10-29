# Helix's north star (2026-10-29)

**Say what you want. Helix does it with you, shows you what it is doing, and lets you take it back.**

Helix stops being "a desktop with an AI hub and an agent API" and becomes an operating system built around one idea that people and
machines share: **actions**. Everything below follows from that, and it is what Apple's approach to agents (`apple-agent.md`) and Helix's
own decisions (one set of tools for people and machines in `mobius-workspaces.md`, real work in `agents.md`, watch-and-help computer use
in `computer-use.md`, local-first models in `helix-models.md`) have in common.

## Four pillars

### 1. One catalogue of actions, for people and machines

Every Helix app, service and Mobius environment publishes what it can do as **actions**: a verb, typed parameters, the things it acts on
(a page, a model, a file, a document, a picture, a task), a risk class, and how to undo it. The person's buttons and an agent's tool
calls are the same actions, recorded in the same history, attributed (`by: you | Mobius | Claude Code | a bot`), undone the same way.

* Settings rows, Models slots, Software installs, the shell's panels, Write's edits, Image's operations and Code's edits are actions.
* Agents of any kind (Mobius's own loop, bring-your-own agents over ACP and MCP, bots) see the catalogue as their tools.
* Driving the screen (the agent display) is how an agent reaches the long tail that has no actions yet, never the first choice.

### 2. One presence, drawn by the system

There is one way to ask, from anywhere: a key, the bar, voice. It knows what "this" is (the focused app reports its current thing) and
answers in place: a result inline, a sheet to confirm, a task that keeps going. While anything acts, the compositor draws a glow at the
edge of the screen that no app can fake or hide, and the work sits in the bar's island with progress, "needs you" and Stop. Mobius, the
app, is where long work lives (the environments: Write, Image, Code, Agents), not the front door.

### 3. Trust you can see

Local first, with the model gateway deciding where each request goes and saying why. Consequential actions are confirmed in plain words
by the trusted surface; everything else runs and offers Undo; every action is in the audit trail; snapshots let a whole task be taken
back. Because of this, Helix can offer more autonomy than other systems (profiles that act without asking, time-boxed runs, bots working
together) instead of less.

### 4. Open by design

Apple keeps things safe by restriction; Helix keeps them safe by making everything visible, reversible and boxed, so it can say yes
where Apple says no. **Power is moved behind a layer, never removed.**

* The action catalogue is a public interface: any app, script or plugin can publish actions and call them (D-Bus, MCP, the `helix`
  command line). Not only "approved" apps.
* Everything you can click, you can script: `helix do settings.set theme.mode dark`, `helix history`, `helix undo 42`.
* Any model, any agent: local, your other machines, any provider; Claude Code, Codex, OpenCode and Hermes-style agents are first class.
* Autonomy up to fully hands-off, chosen by the person, time-boxed, with snapshots and Undo.
* Your data in plain files you own: history, Mobius's memory, skills, automations, routing. No account, no telemetry.
* Extensible surfaces (launcher providers, Control Centre modules, island activities, text actions) on the same kit, so they look native.
* Open protocols between devices (an OpenAI-compatible endpoint, Matrix or Signal), and a compositor that is part of the product.

The discipline that keeps it clean: power never leaks into the everyday path (defaults, the Setup Assistant and the three daily moments
stay simple), one Developer Mode switch reveals the plumbing, and every power feature still uses the same grammar (sheets, plain words,
Undo).

And it looks and moves like modern Apple built it (`modern-apple-direction.md`).

## What changes

| Today | Becomes |
|---|---|
| Agent Surface tier 2: a fixed allowlist of shell commands | The action catalogue (`org.helix.Actions1` on D-Bus, mirrored as MCP tools); tiers become risk classes on actions |
| Several entry points: bar Assistant, Caps Lock act, Mobius key, agent key | One presence; the others become ways to open it |
| Mobius as the place you go to talk | Mobius as the workspace for long work; asking happens anywhere |
| Per-stage operation lists in Mobius (Image `Op`) | The general mechanism, used by every app |
| "Act" as a desktop-settings mode | Gone as a concept: desktop settings are just actions in the catalogue |
| Undo per feature | One history and Undo across apps, plus task snapshots |
| An agent's progress inside Mobius | The island and the glow, system-wide |

## Kept, not changed

The model gateway and Helix Models; the trusted surface and approvals; the agent display and the watch-and-help handoff; bring-your-own
agents in their boxes; local-first privacy; the compositor as part of the product.

## Milestones

1. **Actions v1.** The catalogue type and service (`core/helix-actions`: action, parameters as JSON Schema, entity, risk, undo), published
   by Settings, Models and the shell; the MCP server and Mobius's tool loop read it. One history file with attribution and Undo.
2. **The presence.** One invocation; the focused app's current entity; inline answers and confirm sheets; the compositor's edge glow; the
   island as the home of running work.
3. **Everything is an action.** Software, Files-level operations, Mobius Write/Image/Code operations, notifications; a bridge for other
   apps (`.desktop` actions, MPRIS, portals).
4. **Long work.** Tasks and bots on the catalogue, snapshots and time-boxed autonomy, automations you can see and edit (from
   "every morning…").
5. **Knowing your things.** A local personal index, opted into source by source, so requests can name things the way people do.
6. **The model in every app.** Typed generation from the gateway for any Helix app (summaries, rewrite, extract), local by default.

## Status

* 2026-10-29: **Actions v1** (Settings: find, read, change; one history with who did it; Undo) in the desktop service, the MCP server and
  Mobius's tool loop. **Presence, first parts:** the bar's island shows an agent's change in plain words with Undo, and the compositor lights
  the screen edges while an agent acts (`HelixSetGlow`, `compositor/src/helix/glow.rs`: pink, blue, purple and orange light fading inward,
  under trusted surfaces, still rather than animated). Nested: `tools/nested/ui-suite.sh actions`, `mobius-actions`.
* 2026-10-29 (later): **One place to ask, first step.** The launcher (`Mod+space`) finds apps and settings by what they do (the actions
  catalogue's search): Enter flips a switch setting there (the person's change, recorded with Undo) or opens the setting's page; anything
  typed can be asked of Mobius (the last row, the only one when nothing matches), which opens Mobius in the bar with the words. The
  "act …" prefix still lists desktop actions. Nested: `tools/nested/ui-suite.sh presence`.
* 2026-10-29 (later): **Actions from any app** (manifests + `org.helix.ActionProvider1`, undo calls kept in the history) and **Models**
  as built-in actions; `helix`, the command line.
* 2026-10-29 (later): **Concept pass.** "Act" is no longer a mode: the bar has one box, "Ask", and Helix decides whether a request is
  something to do on the desktop or a question (`helix_shell_model::ask`: a request that starts with a verb of doing, after a polite
  opening, is done; "Do it" turns an answer into doing); the launcher's Ask row uses the same decision. People see "Desktop" for the task
  that acts on the desktop, "Agent workspace" (was agent desk) and "Agent screen" (was agent display); stale texts ("settings still use
  Act", "Act never uses a cloud model") are gone.
* 2026-10-29 (later): **Writing Tools** (intelligence in place, first part): select text in any app, open Writing Tools (the launcher, or a key
  bound to `helix-shell "writing"`): Proofread, Friendly, Professional, Concise, Summary, Key points, Explain, or ask about it. The answer
  streams from the small, quick Summaries model on this computer; Replace puts a rewrite back over the selection in the window that had it
  (focus is handed back first; never multi-line into a terminal), Copy copies. The selection is the primary selection, else a copy made in
  that window with the clipboard put back (never the copy key in a terminal). `helix_shell_model::writing`, `shell/…/writing.rs`.
  Nested: `tools/nested/ui-suite.sh writing`.
* 2026-10-29 (later still): **the Magic Pointer**: point at anything and press Mod+/ (`helix-shell "point"`). The compositor notes where the
  pointer was and the window under it (`HelixPointContext`); the shell opens a glass bubble at that corner of the pointer with "What's this?",
  "Copy text" (read from the picture, put on the clipboard), "Ask about this…", Writing Tools on the selection (already running), and the
  actions apps add that fit (`"selection"` / `"apps"` in a manifest; run as the person, with the app's own Undo). The window is captured only
  when something is picked: that window alone (the bubble is never in it), cut to the patch around the pointer, kept in memory while the
  bubble is open, on the Vision task (labelled "On this computer" or "Answered online" from the routing). An answer takes the menu's place,
  with a back button. `helix_shell_model::pointer`, `shell/…/pointer.rs`, `compositor/src/helix/point.rs`. Nested: `ui-suite.sh pointer`.
* 2026-10-29 (evening): **notification summaries**: the shade stacks notifications by app; a stack of two or more is one card (the
  edges of the others peek out below) with a one-line summary by the small Summaries model ("Pat and Sam agreed on Friday after lunch"),
  written when the shade opens and again only when the stack changes. A click opens the stack, which can be cleared at once. Only on this
  computer: if the Summaries task is routed online there are no summaries (a notification is never sent anywhere to be summarised); the
  notification is data to the model, never instructions. Settings → Notifications → "Summarize notifications" turns it off.
  `helix_shell_model::notification_summary`, `notify.rs`. Nested: `ui-suite.sh notification-summaries`.
* Next: the Setup Assistant and settings audit.

The roadmap's other sections (foundations, Settings pages, apps) continue, but new work there is built as actions from the start.
