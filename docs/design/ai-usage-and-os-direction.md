# What people use AI for, and what that means for Helix

Research notes written 2026-10-23 from public sources (listed at the end), then the direction they point to for Helix as an operating system and a set of
applications that use AI. Figures are as reported by the sources or by search-result summaries of them; where I could not open the primary page I say so.
Treat the numbers as orders of magnitude, not as measurements of Helix's users.

## 1. What people actually do with AI

**Consumers (ChatGPT, 1.5 M conversations, OpenAI Economic Research with an NBER economist; Microsoft Copilot, 37.5 M conversations).**
* Three topics dominate: **practical guidance, seeking information, and writing**, together nearly 80% of ChatGPT conversations. Non-work use grew from
  53% to over 70% of messages. The paper frames use as **asking** (advice, information), **doing** (producing an output) and **expressing**.
* Microsoft's split is the most useful one for an OS: **on a desktop AI is a co-worker** (work and career is the top topic in business hours, programming
  spikes on weekdays); **on a phone it is a life adviser** (health and fitness is the top topic every hour, more personal and late-night). About one in five
  conversations were health-related, and one in seven of those about someone else. Use broadened over 2025 from developers to everyone.
* Anthropic's Economic Index (to June 2026): **93% of conversations produce an artifact**, led by explanations (17%), documents and reports (15%), guidance
  (11%); personal use favours explanations and recommendations, work favours documents; weekend personal use is ~50% against ~35% on weekdays.
  Augmentation (the person steers) is about 52% and automation (delegated) about 45% on chat, and **agentic, long-running sessions are growing quickly**:
  a typical Claude Code session is one human prompt where chat takes about thirteen rounds.

**Developers (Stack Overflow 2026).** 73% of those using AI assistants use them daily; agent use rose to about 59% by April 2026; 62% are at least
somewhat positive (71% among daily users); the top reasons to switch tools are output quality. Skepticism rises as agents get more autonomous.

**Everyday AI on a PC (vendor-reported).** Voice dictation with AI clean-up (roughly three times faster than a phone keyboard in a Stanford study),
summarise / rewrite / write in the editor, natural-language search over files, apps and settings, and "explain this screenshot". Microsoft lists Copilot as
its most-used productivity app on Windows.

**Trust and control.** 81% of surveyed consumers are concerned about AI accessing personal data; 48% name privacy their top concern; only ~31% trust AI to
make decisions independently and 44% fear unauthorised autonomous actions; ~51% want AI features that are customisable or limitable. The Windows Recall
episode (screenshots of credit cards and passwords stored in clear text, then withdrawn, made opt-in, encrypted, gated behind authentication) is the
cautionary tale for any "AI remembers your screen" feature.

**Agents are not yet reliable.** On OSWorld 2.0 (108 long workflows, ~1.6 human hours each) the best system completes about 20.6% end to end; the failures
named are losing track of constraints, missing information that arrives mid-task, **guessing instead of asking the person**, and skipping verification.
And people stop reading approval prompts: Anthropic reports users approve ~93% of Claude Code permission prompts, and auto-approve use climbs from ~20% of
sessions for new users to over 40% at 750 sessions.

## 2. Where the platforms are going

* **Windows (Build 2026):** "an OS for agents". Agents get their own identity and a separate workspace session, run inside **MXC** policy containers (files and
  network declared up front, enforced at runtime, audited per identity), with MCP and local models. *(Helix already has the same shape: Agent Surface tiers,
  agent desk, task sandbox policies.)*
* **Apple:** on-device models first, **Private Cloud Compute** when needed, Siri rebuilt as a chat assistant, native agentic coding in Xcode. Privacy is
  the first rule, escalation is visible.
* **Google:** Gemini folded into Android and ChromeOS as an agent that plans, delegates to sub-agents and uses several models.
* **Linux:** Ubuntu says implicit AI (better speech to text) and opt-in explicit agentic features, on-device through sandboxed snaps. Fedora's "AI
  Developer Desktop" ran into sharp community opposition, with the privacy rule that nothing is pre-loaded to watch you and nothing talks to the cloud by
  default. GNOME and KDE are wary on contributions. A Linux audience values local, opt-in and inspectable.

## 3. What this says Helix should be

**The three jobs people give AI, in order of frequency:** get an answer or advice; produce a piece of writing or code; do a task for them. Helix should be
excellent at the first two *without any setup or approval*, and trustworthy at the third.

1. **Make asking instant and ambient.** One key, from anywhere, knows what you were just doing (the summon note already does this), answers in a
   moment, streams. Voice in and out is a first-class input on a desktop, not a gimmick: dictation with clean-up, a spoken question.
2. **Make producing an output the centre of the app.** The result is an artifact (a document, a draft, a snippet, a table, a chart) you can edit, copy,
   save and reuse, not a chat bubble. The 93%-produce-an-artifact number is the argument for Mobius's stage and for AI inside the apps (editor,
   mail-like text fields) through Quick Insert.
3. **Make doing safe by construction, and rare in its prompts.** Approval prompts do not scale (93% approved). So: tiered by risk, backed by hard limits
   (policy sandbox, agent desk, review-on-copy), explaining what, why, and how to undo; ask only for what is irreversible or leaves the machine; show a
   diff or a plan up front instead of a stream of "Allow?". Agents must *ask when unsure* (the commonest failure) and *verify* before saying done.
4. **Local and private by default, escalation visible.** The Apple pattern, our gateway's hard rules, and what Linux users expect. A routing reason is
   shown ("answered on this computer: it contains a picture"). Never an always-on screen recorder; any memory is opt-in, inspectable, deletable, encrypted
   (the Recall lesson).
5. **Control is a feature.** A single place to see and change what AI is allowed to see and do (Settings → Privacy & agents, Helix Models → Routing,
   Access), per app and per agent, with an audit trail a person can read. Half of people want to limit AI features: let them, per feature.
6. **Phone-shaped use will reach the desktop.** Personal, health and advice questions move to the device in your pocket; Helix should expect phones as
   clients of the gateway (`helix-models.md`, pool notes) and not assume work-only use. Privacy defaults matter more here.

## 4. Product direction

| Pillar | Direction | State |
|---|---|---|
| **Ask** | Summon from anywhere with context; streaming; voice in/out; instant start (engine on GPU, model kept warm); a quick surface (bar/Quick Insert) distinct from the long conversation | Summon, streaming, dictation, GPU engine done; read-aloud and a spoken loop not built |
| **Make** | Artifact-first Mobius (documents, code, charts, tables as editable stage objects); AI inside text fields system-wide (rewrite, summarise, fix) via Quick Insert; edits as proposals | Stage and inline edits done; system-wide text actions partly (Quick Insert) |
| **Do** | Agent tasks with plan-first, review on a copy, policy sandbox, undo, agent desk and computer use with human hand-off; ask-when-unsure and verify-before-done as measured behaviours | Built (`agents.md`, `act-undo.md`); verify/ask behaviours not measured |
| **Trust** | Local first, visible routing reasons, grants per app and agent, readable audit, opt-in memory only, encrypted local stores | Gateway rules and audit line done; reason chip, readable audit page, memory controls to do |
| **Control** | Helix Models (backends, routing, access, activity); Privacy & agents; per-feature switches | Models app v1 built; Access and Activity to do |
| **Everywhere** | Phones and other machines as clients/peers of the gateway; same rules everywhere | Noted, not built |

### What to build next (ordered by value over effort)

1. **Policy router with reasons** and the routing log (`model-routing.md` step 1); show the reason in Mobius. It is also the trust feature.
2. **Verify and ask-when-unsure for agent tasks:** a final verification step before "done", and a rule that the planner asks the person rather than guessing
   when a constraint is ambiguous. Build an eval for both (OSWorld's failure list is the checklist).
3. **Approval design pass:** move from per-action prompts to plan-level approval plus risk tiers; measure approve rates and surface "this will leave the
   machine / delete" only. Keep the trusted-surface protections.
4. **System-wide text actions:** select text anywhere, one key: rewrite, summarise, explain, translate, fix. Highest-frequency "doing" for most people.
5. **Voice loop:** push-to-talk dictation with clean-up exists; add read-aloud with cancel and a quick spoken question.
6. **Readable audit and privacy dashboard:** what each app and agent saw and did, per day, with delete. (Also answers the Recall-style fear before it exists.)
7. **Memory, opt-in:** explicit notes and chosen folders (`knowledge.md`) before anything automatic; never a screen recorder.
8. **Helix Models:** downloads, Activity, Access.

### What not to do

> Stance update (2026-10-23): the owner is AI-forward and wants to test the boundaries; see `ai-forward-platform.md`. The list below is the *default for
> ordinary users*; each item becomes a configurable setting with rollback and audit (Lab, graduated autonomy), not a prohibition on the platform.

* No always-on capture of the screen, keystrokes or microphone. No data leaves the machine without a visible, revocable choice.
* No AI that is on by default where a person did not ask for it; every feature has a switch; nothing is pre-loaded to watch.
* No wall of approval prompts, and no "YOLO mode" as the answer to approval fatigue: reduce the prompts by raising the floor (sandbox, review on copy),
  not by teaching people to click through.
* No claiming agents are reliable. Say what they did, what they checked, and what they could not do.

### How we will know

Instrument locally (never uploaded): time to first answer, share of turns answered locally, approve/deny/edit rates for agent steps, tasks completed
versus abandoned, undo use, regenerate and "ask a stronger model" rates. These are the Helix equivalents of the studies above, and they feed the routing
and approval work. Anything shared would be an explicit, inspectable export.

## Sources

* OpenAI and NBER, *How People Use ChatGPT* (nber.org/papers/w34255; summary via search results, the OpenAI page itself was not readable here).
* Microsoft, *It's About Time: The Copilot Usage Report 2025* (37.5 M conversations; via GeekWire, Axios and Microsoft Research summaries).
* Anthropic, *Economic Index report: Cadences* (anthropic.com/research/economic-index-june-2026-report) and related Economic Index posts.
* Stack Overflow, *2026 Developer Survey* and retrospectives (stackoverflow.blog, Oct 2026).
* Consumer surveys (Shift Browser 2026 AI Consumer Insights, Goodwater 2026, Talker Research), as reported by trade press; sample sizes vary.
* Microsoft Build 2026 coverage (Visual Studio Magazine, Windows Developer Blog "Windows platform security for AI agents", MXC).
* Windows Recall coverage (ITPro, Techfinitive, Decrypt, The Register).
* OSWorld 2.0 (arXiv 2606.29537) and leaderboards.
* Approval-fatigue write-ups citing Anthropic (2026): the 93% approval rate and auto-approve growth (secondary sources; not verified against the original).
* Linux distribution coverage: The Register (Fedora and Ubuntu AI support, May 2026), LWN (May 14, 2026), Fedora Discussion on the AI Developer Desktop.
