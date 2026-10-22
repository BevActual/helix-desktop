# Computer use: agents operate a screen, the person can watch and help

Status: design, 2026-10-21. Nothing here is built except the quiet window picture (`HelixPeekWindow`, below). It follows the person's direction that agents
should have **computer use** (see the screen, move, click, type) and that the person can **switch in to observe what they are doing and help when
they are stuck** (filling in a form, signing in, a captcha, a decision only a person can make), in the manner of the computer-use agents people already
know (Grok's bot, Muse). It replaces the "semantic only, no typing, no pixels" recommendation in `synthetic-input-threat-model.md`: pixel input and typing
are wanted. The rest of that threat model still binds: it says what has to stay true.

## What the person gets

1. **Watch.** One click from the bar's island ("Agent is working on …") opens a live view of what the agent sees and does: its pointer, its clicks, the
   text it types, and a running line of what it is trying to do. The person can leave it in a window, or take it fullscreen.
2. **Take over.** Moving the mouse or pressing a key in that view **pauses the agent at once** and gives the person the keyboard and pointer. Nothing is
   lost: the agent waits where it was. "Hand back" resumes it with a fresh look at the screen.
3. **Be asked for help.** The agent can say "I need you: sign in to the portal" (a `request-help` act with a reason). It stops, the island and a
   notification say so, the view opens on the right window. The person does the step, presses "Done, carry on", and the agent resumes. **Secrets are
   never typed by an agent**: a password field is the person's, always.
4. **Stop.** The kill switch in Privacy & agents, and Escape in the view, end the session immediately.

## How the agent acts: two architectures, one recommended

### A. An agent display (recommended)

The agent works on **its own display**: a nested Helix compositor (niri has the winit backend for this) whose only clients are programs the agent
launched, started with that session's `WAYLAND_DISPLAY`. The person sees it as **one window** on the agent desk (the "Agent Display" viewer). Input and
vision use things this repository already proves in tests:

* the agent's clicks and keys are ordinary virtual-pointer and virtual-keyboard requests to the **nested** compositor's socket (`wl-pointer` and `wtype`
  are the same mechanism);
* its vision is the nested compositor's own screenshot (`desktop-capture` aims at it);
* the person's own input into the viewer window is forwarded by the winit backend, so helping is simply using the window; the nested compositor marks
  those events as physical, and **physical input pauses the agent**.

Why recommended: it **removes the dangerous half of the threat model by construction.** The agent cannot reach the person's windows, Settings, the
approval dialog, a terminal the person owns, the lock screen or an authentication dialog, because none of them exist on its display (T1 to T4, T6 are
not "refused", they are unreachable). There is no off-limits list to keep right, and no window-identity check that a bug can bypass. The cost is that the
agent's programs are separate from the person's session: no shared clipboard unless Helix carries it on purpose (a visible, approved "copy to agent
display" action), and more memory for a second compositor.

### B. Injection into agent-desk windows of the person's own compositor

Windows the agent opened sit on the agent desk workspace (today's design). The compositor would deliver pointer and key events straight to one target
surface and refuse any other window. It keeps one session, but it has to be **right** about every refusal (T1 to T6), the person's pointer and keyboard
focus must be saved and restored around each injected event, typing needs a per-window path that is not a virtual keyboard
(`trusted-surface.md` explains why a virtual keyboard cannot be filtered), and windows on an inactive workspace are rendered at a reduced rate. It is
the harder one to make safe and the harder one to test.

**Recommendation: A.** The agent desk workspace stays: it holds the viewer window (and any window the agent opens that is not computer use, as today).

## Hybrid compute: local, LAN and cloud models

Mobius is hybrid by design (`inference-fabric.md`: local slots, LAN endpoints, cloud), and agents follow. Computer use is where it matters most, because a
screenshot is the most private thing a model can be sent. The ideas, with what they take from Hermes Agent (Nous Research: a learning loop with
agent-curated memory and self-made skills, cross-session recall, any model endpoint, many terminal backends from local to Docker, SSH and serverless
cloud, one gateway to messaging apps, MCP) and what Helix adds:

* **Per-agent routing with a policy, not a toggle.** Each agent has a policy: *Private* (local only), *Balanced* (local by default, cloud for what local
  cannot do, asked first), *Best* (cloud planner). The router sends the step to the cheapest model that is allowed to see it.
* **A planner and a grounder.** The planner (often a cloud model) decides what to do from a small, scaled screenshot and the task; the **grounder**
  (local) turns "the Export button" into coordinates on the full-resolution screen. Fewer and smaller pictures leave the machine, and the click
  position never depends on a remote model seeing every pixel.
* **What a cloud model can see is bounded by the architecture.** Under option A the model sees the agent display and nothing else: the person's windows,
  files and notifications are not on it. That is what makes cloud acceptable for computer use at all.
* **Secrets stay local.** A password field, a payment form or a page the grounder classifies as a sign-in is sent as a blurred region (or not at all),
  and the step is handed to a local model or to the person.
* **Always visible.** The viewer and the island carry a small cloud badge while a cloud model is in the loop; a session log says how many pictures
  were sent, to whom, and what policy allowed it. "Stop sending" switches the session to local.
* **The agent display is a place, not one implementation.** v1 is a nested session on this machine. The same viewer can later attach to a remote
  place (a VM or a cloud desktop), the way Hermes has terminal backends; that is also how an agent keeps working when this computer is asleep.
* **Memory and skills are local and the person's.** An agent's memory and the skills it writes for itself live in the knowledge store
  (`knowledge.md`), are readable and deletable in Settings, and are never uploaded as a side effect of using a cloud model.
* **Reach the person where they are.** A help request can arrive on a phone (a gateway, later) with the last picture and one question; anything that
  needs real control still brings the person to the viewer.

**A standing rule to settle.** Until now *Act* (the desktop assistant's control of the person's own desktop) never uses a cloud model. That stays: Act
sees the person's desktop. Computer use on an agent display is a different surface (it sees only what the agent put on its own display), so it is
proposed as the one place a cloud model may be used for control, per agent, opt-in, visible, and off for the person's own windows.

## What stays from the threat model, whichever way

* The agent only ever acts on its own display or windows it opened (never on the person's), and **never** on a trusted surface, the lock screen, the
  greeter or an authentication dialog. Under A this is structural.
* Every input is **audited**: agent, action, target, coordinates or text length (the text itself only when the person chose to log it), and a
  before/after capture reference. The client that is acting cannot read the audit.
* **Rate limits** per grant and per second, bounded batches, an arming delay for the first action of a session.
* **Taint (T5):** after the agent reads private content (a page, a notification, a clipboard) the next input waits for a confirmation drawn by the shell
  that names what it is about to do, not the model's reason. Agent Surface already draws these.
* The grant is a new capability, **`computer-use`**, in the grants table, with the tiers and arming that every other risky act has. It is off by default
  and needs the person's approval the first time per agent, in the shell's dialog.
* A visible indicator while an agent drives: the island ("Agent is working"), a frame on the viewer, and the existing kill switch.

## Phases (each useful alone)

0. **Done:** `HelixPeekWindow` (compositor): a picture of one window written to a file with no clipboard copy, no notification and no event, so a live
   view does not spam the person. Useful for the viewer under either architecture.
1. **Watch.** The agent desk viewer and the island entry; live line of the agent's latest step from the activity feed. Under A: the Agent Display
   window. No input yet.
2. **Help request.** The `request-help` act, the "needs you" island state, the notification, "Done, carry on". Works with today's semantic acts and
   for every later phase.
3. **Agent input, scoped.** `computer-click`, `computer-move`, `computer-scroll`, `computer-type`, `computer-key`, `computer-screenshot` as acts bound to
   the agent display, classified in `msg-risk.toml` (typing and clicking are *confirm* tier on first use of a session, then rate limited), with the
   physical-input pause and the kill switch. Nested tests: an attacker run proves the agent cannot reach anything outside its display; a person's mouse
   move pauses it mid-sequence; a password field is refused for typing.
4. **Take over and hand back**, with the agent re-reading the screen on resume.
5. **Polish:** a copy-in/copy-out action for the clipboard, file hand-off, multiple agent displays (one per agent), recording a session to replay.

## Decisions for the person

1. **Architecture A (agent display) or B (agents operate windows on the real desktop)?** Recommended: A.
2. **Typing in the first release?** Recommended: yes, except into password fields (detected by the toolkit's accessibility role where it exists, and by
   the person's explicit "this is a secret" when it does not).
3. **Where does the person watch?** Recommended: the viewer window on the agent desk plus the island entry; fullscreen on a second monitor when there is
   one.
