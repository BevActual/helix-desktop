# Tier 3 (synthetic input): threat model

Status: design, 2026-10-16. Nothing here is built. `agentic-os.md` lists tier 3 as phase 2 and blocked in v1; this note says what has to be true
before that changes, so a later implementation is judged against it rather than argued from scratch. It builds on `trusted-surface.md`, which
already protects the approval dialog from synthetic input; this note is about giving agents synthetic input on purpose.

## What tier 3 would be

An agent, through Agent Surface, moves a pointer, clicks, scrolls and types into a window it did not build: the way a person would, for
applications that offer no semantic door (no panel command, no setting, no D-Bus method). Two shapes, very different in risk:

| Shape | What the agent says | What Helix does | Risk |
|---|---|---|---|
| **Semantic (accessibility tree)** | "press the button named Export in window W" | finds the node by role and name through AT-SPI, calls its action | the agent names a control; it never aims at pixels |
| **Pixel** | "click at (812, 440)", "type this" | injects a virtual pointer or keyboard event | the agent can hit anything drawn there, including what it did not intend |

Everything below applies to both; the pixel shape is strictly worse and is not the first thing to build.

## Why this tier is different from the others

Tiers 1 and 2 are **enumerated**: each command is a named verb with known effects, classified in `msg-risk.toml`, and Act now records what it
changed and can undo it (`act-undo.md`). Tier 3 is **unbounded**: a click is whatever the thing under the pointer does. So:

* **Tier 3 is the sum of every other tier.** An agent that can click can open Settings > Privacy & agents and switch its own grant on, install a
  package in Software, type a command into a terminal, or press Allow in any dialog that is not a trusted surface. A grant that says "input" is
  therefore a grant for everything the window under the pointer can do, unless scoped.
* **The per-command risk table cannot classify it**, and **undo cannot cover it** (there is no before and after value to restore).
* **It turns reading into acting.** `observe-private` content (a screenshot, a notification, a clipboard, a web page) can carry instructions
  (prompt injection). Today that content can only steer the model toward enumerated, tiered verbs. With tier 3 the same text can steer a click.

## Assets

1. The person's approval (the confirm tier): a decision made by the person, never by the agent.
2. The grant table and the kill switch (Settings > Privacy & agents; `agent_activity`).
3. Anything an application the person is signed in to can do (mail, banking, a terminal, a password manager, the package manager).
4. Secrets on screen or in focus: passwords being typed, authentication dialogs (`helix-askpass`, polkit), tokens in a terminal.
5. The person's attention and trust: they must be able to see that an agent is driving, and stop it.

## Adversaries

| Adversary | Position | Capability |
|---|---|---|
| **Injected content** | text the model reads: a web page, a notification, a document, a screenshot of any of those | steers a well-behaved, granted agent toward actions the person did not want |
| **Over-eager agent** | a granted agent that misreads a screen | clicks the wrong thing, repeatedly and quickly |
| **Malicious MCP client or helper** | a granted client that is itself hostile | requests input beyond what it needs; races the person |
| **Hostile same-user process** | any program running as the person | already owns the session (`agent-and-inference.md`: not defended here); tier 3 must not hand it a better door than it has |

## Threats and what answers them

| # | Threat | Answer, in order of strength |
|---|---|---|
| T1 | The agent approves its own request (clicks Allow) | **Already closed** for the dialog: `trusted-surface.md` drops virtual pointer input and any key-made click at the trusted surface. Tier 3 must keep that: injected events are never delivered to a trusted surface, with no setting to turn it off |
| T2 | The agent widens its own grants (clicks in Settings > Privacy & agents, the approvals switch, the kill switch) | those windows are **off limits to synthetic input**, by window identity (app id and exe), enforced where the input is injected, not in the agent's prompt |
| T3 | The agent runs arbitrary code (types into a terminal, runs a command in a package tool) | terminals and shells are off limits by default; typing is the most dangerous action and gets its own rule (T6) |
| T4 | Authentication prompts: the agent types or clicks in a password, polkit or `helix-askpass` dialog | those dialogs are off limits; an agent never receives or supplies a secret |
| T5 | Injected content steers the click | a screen the agent just read as `observe-private` taints the next input: the first input action after private content needs a human confirmation naming the target window and the action, not the model's reason |
| T6 | Typing goes somewhere other than intended (focus moved between the decision and the keystroke) | input is **bound to one window**: it names a window id and is refused if focus is elsewhere or the window changed; no global key chords, no compositor bindings |
| T7 | Speed: many clicks before the person can react | rate limit per grant; each batch bounded; first use in a window waits for a human decision (arming delay, as for confirm) |
| T8 | The person cannot tell an agent is driving, or cannot stop it | a visible indicator while input is active (the bar island, same place Act shows progress); the existing kill switch ends input immediately; any physical input from the person pauses the agent |
| T9 | Nothing can be reviewed afterwards | every input action is audited with the target window, the control (semantic) or coordinates (pixel), and a before and after capture reference; the audit is not readable by the granted client |
| T10 | A cheaper path than tier 3 is used where one exists | the agent must prefer a semantic act (tier 2) when the command exists; input is for what has no door |

## Scope: the rule that makes the rest workable

**Input is granted per window, not per agent.** A grant names an application (identified by its verified executable, as grants already identify clients)
*and* the person approves, the first time, "this agent may operate this application's windows". Without that, the grant is all or nothing.
Off-limits windows (T2 to T4, the trusted surface, the lock screen, the greeter) are refused whatever the grant says.

**The agent desk is the natural sandbox.** Windows opened on the agent desk (`agentic-os.md`) are the ones an agent made. The first
implementation should allow input **only to windows on the agent desk** that the agent itself opened. That removes most of T2 and T3
without extra machinery: the agent cannot reach the person's own windows at all.

## Order of work (gates, not a schedule)

Nothing below tier 3 itself is blocked on the next step; each step is useful alone.

1. **Close the keyboard hole noted in `trusted-surface.md`.** `zwp_virtual_keyboard_v1` events are delivered straight to the focused client and
   cannot be filtered in the compositor. The dialog refuses to be allowed by any key, which is enough for the dialog; it is not enough for
   "off-limits windows" (T2, T4), because typed input reaches whichever window has focus. Tier 3 typing therefore must not use a virtual keyboard
   at all: it goes through an input method or the accessibility API *aimed at one window*, which the compositor or toolkit can refuse by window.
   This is the first thing to prove, with a nested test, before any of the rest.
2. **Window identity in the compositor** (`niri msg` already lists windows with app id and pid): an injection API that takes a window id and
   refuses off-limits windows. Tested with the attacker program the way `trusted-attack-test.sh` tests the dialog: every off-limits case must fail.
3. **Semantic first:** AT-SPI actions on named controls of agent-desk windows. No pointer, no keys. This covers most uses and keeps the agent
   naming controls instead of aiming.
4. **Confirmation and taint (T5, T7):** the first-use decision, and a confirmation after private content, drawn by the shell, not the agent.
5. **Indicator, pause and kill (T8), audit (T9).**
6. **Only then**, if semantic is not enough: pixel click and typing, still bound to one window, still rate limited, with a nested test that a
   click aimed at an off-limits window or a moved window is refused.

## Tests this design asks for (written before the code)

* An attacker with a tier-3 grant for application A tries each of: click in Settings > Privacy & agents, in the approval dialog, in a terminal,
  in an authentication dialog, in the lock screen: all refused.
* Focus moves between the decision and the keystroke: the keystroke is refused, not delivered to the new window.
* Private content was read, then an input is requested: it waits for the person; with no answer it times out and nothing happens.
* The kill switch, and physical mouse movement, stop an input sequence mid-way.
* A request for an input command with no grant for that window gets a refusal that names the missing grant.
* Control run (the check is not vacuous): with each protection switched off in a test build, the same attack succeeds.

## Decisions for the person

1. **Agent-desk only first** (recommended), or allow the person's own windows from the start with the per-window first-use decision?
2. **Typing**: allow it in the first release, or semantic actions only (press, select, toggle) until the input-method path is proven?
3. **Pixel input at all**: build it, or hold the line at semantic and treat anything else as a missing accessibility label to fix in the app?

My recommendation is yes, semantic only, and no, in that order: agent desk only, no typing, no pixels. That ships something useful (operate
applications the agent launched, through their accessibility tree) with the smallest new power, and every later step has to clear the tests above.
