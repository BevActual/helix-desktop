# How Apple would build an agent, and what that means for Helix

Apple has not shipped an open-ended "agent". What it has shipped (App Intents, Siri with personal context and on-screen awareness, Writing
Tools, Shortcuts with models in them, Visual Intelligence, the Foundation Models framework, Private Cloud Compute) shows how it would build one.
Read as a design, it is a different bet from the one most agents make, and it fits Helix's own pieces unusually well.

The bet: **the agent is the operating system, acting through what apps say they can do, on the person's own data, privately, visibly and
reversibly.** Not a chatbot that drives a mouse.

## Ten principles

1. **Actions, not pixels.** Apps declare their verbs and nouns (App Intents: "send message", "add to list"; entities: a contact, a note, a
   playlist) with typed parameters and a human description. The agent composes those. Clicking through a screen is the last resort, never
   the method.
2. **The system is the agent.** One presence, reachable from anywhere by voice, typing or a key, not an app you go to. It has no persona to
   chat with; it has things it can do.
3. **Personal context stays personal.** An on-device semantic index of the person's own things (messages, mail, files, calendar, photos)
   answers "the document Sam sent last week". It is built and queried on the device.
4. **On-screen awareness through meaning.** "Add this to my notes" works because the frontmost app tells the system what "this" is (the
   entity on screen), not because a model reads a screenshot.
5. **Private by architecture, not by policy.** On the device first. When a bigger model is needed: a cloud that keeps nothing and can be
   verified (Private Cloud Compute). A third-party model only when the person agrees, request by request.
6. **Visible.** When the agent listens or acts, the whole screen says so (Siri's edge glow), drawn by the system so no app can fake it or
   hide it. Work in progress lives in one place (the Dynamic Island, a Live Activity) with a way to stop it.
7. **Consequential means confirmed, in plain words.** "Send 'Running late' to Sam?" with the real recipient and text, not "Allow action?".
   Reversible things just happen and offer Undo.
8. **Narrow and reliable over broad and flaky.** A model fills in parameters; deterministic code does the work. If it cannot do something
   well it says so instead of trying.
9. **Requests become automations.** "Every morning, do this" turns into a Shortcut the person can see, edit and turn off. Models can be a
   step inside it (Use Model).
10. **Every app gets the model.** A typed, on-device model API (guided generation into a type, tool calling) for any developer, free, so
    intelligence shows up inside apps (Writing Tools everywhere), not only in the assistant.

## Where Helix already stands

| Apple's piece | Helix today |
|---|---|
| App Intents | Agent Surface tier 2 (semantic acts) and the Helix MCP server: a fixed allowlist, not yet a catalogue apps publish |
| System presence | The bar Assistant, the Mobius key (`Mod+space`), the agent key (`Mod+Ctrl+a`); several entry points, no single presence |
| Edge glow | Nothing system-drawn; the compositor *could* draw it (helix-niri is ours) |
| Live Activity | The bar's island already shows work in progress (assistant, approvals) |
| Confirmation in plain words | Approval dialogs (trusted surface), `act-undo.md` (restore / none classes) |
| Personal index | `knowledge.md` (project knowledge); no system-wide personal index yet |
| Private Cloud Compute | The model gateway: local first, trust labels (this computer / your network / internet), Act never leaves the computer |
| Shortcuts + Use Model | Mobius Scheduled prompts and agent tasks; no editable automation object |
| Foundation Models API | `org.helix.Inference1.Action` with grammar-constrained output: the core exists, not yet a typed API for apps |
| Last-resort screen use | The agent display and agent desk (`computer-use.md`), with the person able to watch and take over |

Helix has more of this than it looks: the gateway, the trusted surface, the compositor and the island are the hard parts. What is missing
is the **catalogue of actions** and the **single visible presence** that ties them together.

## What to build, in order

1. **Helix Intents.** Each Helix app and service publishes what it can do: a manifest of intents (verb, typed parameters, a one-line
   description, risk class from `act-undo.md`) and entities (a Settings page, a model, a backend, a package, a chat, a file), served on
   D-Bus (`org.helix.Intents1`) and mirrored into the MCP server. Settings, Models, Software, Mobius and the shell first; then a bridge
   for other apps (`.desktop` actions, MPRIS, portals). Agent Surface tier 2 becomes "whatever is in the catalogue".
2. **One presence, system-drawn.** A single invocation (a key, the bar, voice) that opens the same assistant everywhere, and a
   compositor-drawn glow at the screen edge whenever an agent listens or acts. Because the compositor draws it, no window can fake or
   hide it. Running work sits in the island with progress and Stop.
3. **This, here.** The focused app reports its current entity ("Settings › Network › Home WiFi", "Mobius › this chat", "Files › report.pdf")
   so "this" in a request means something exact. Screenshots stay a fallback.
4. **Plain-word confirmation and Undo everywhere.** Consequential intents show the exact effect in the trusted surface. Reversible ones run
   and post Undo. Every act goes into the audit trail, as now.
5. **A personal index, opted into source by source.** Local embeddings over the folders, mail and notes the person chooses, queried only
   on this computer, so requests can name things the way people do.
6. **Automations you can see.** "Do this every morning" produces an automation object (trigger, steps, model steps) shown in Mobius
   Scheduled, editable and switchable off, instead of a hidden prompt.
7. **The model for every app.** The gateway's grammar-constrained output, offered as a typed API (a JSON schema in, a value out, tools
   allowed), on the local model by default, so Helix apps add intelligence inline (summaries in Notifications, rewrite in any text field).
8. **Computer use last.** The agent display stays for what has no intents, now clearly the fallback, with the same glow and island.

## How this sits with Helix's AI-forward stance

Apple would hold some of these back; Helix does not have to. The same pieces carry more power when the person chooses: profiles that allow
consequential intents without asking (with Undo and the audit trail), cloud models for planning while acting stays local, and coding
agents in their boxes with the catalogue as their tools. The Apple lesson is about the *shape* (intents, presence, visibility, privacy,
undo), and that shape is what makes more autonomy safe to offer. See `ai-forward-platform.md` and `act-undo.md`.
