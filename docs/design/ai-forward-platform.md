# AI-forward platform: what Omarchy and its neighbours do, and where Helix goes further

Written 2026-10-23. The owner is AI-forward and wants to test the boundaries that make Helix more powerful, so this note studies the Linux systems
that already treat AI agents as the primary way to use the machine, and turns it into a plan. (Companion to `ai-usage-and-os-direction.md`, which is
the more cautious, consumer-facing reading of the same research: where the two differ, *this* note sets the stance: power is available, safety is
rollback and audit, not absence.)

## Omarchy (DHH, Omacom Foundation)

Arch + Hyprland "omakase" desktop that became an agent-first distribution.

* **Adoption is large:** Omarchy 4 "Quattro" (14 Aug 2026) passed 100 000 ISO downloads in a week and 200 000 in 18 days; the Omacom Foundation launched
  21 Aug 2026 with $8 M (about $15 M committed within two weeks, with 1Password and 37signals as sponsors). There is real appetite for an AI-forward Linux.
* **Bring your own agent, pre-wired.** Ten agent CLIs (Claude Code, Codex, OpenCode, Gemini CLI, Copilot CLI, Grok, Crush, Pi, Oh My Pi, OpenRouter's
  Ori) arrive as lazy stubs in `~/.local/bin` that fetch the real tool on first run; one default is chosen at setup, one command changes it; short aliases
  (`c`, `cx`, `a`); a global key (Super+Shift+Ctrl+A) starts the default agent in the current directory; themes sync to the agents. It does not build an agent.
* **Agents are part of the shell.** The bar has an agents panel: plan limits, share of the weekly budget used, session windows, tokens by day and model
  (input / output / cache), optionally aggregated across machines through synced JSON. When a process segfaults, a notification offers to **hand the core
  dump to your agent** (`diagnose-crash` skill).
* **A skill teaches agents the desktop.** An "Omarchy skill" explains how to customise (waybar, themes) and where the line is: `/usr/share/omarchy` is
  package-owned and read-only, `~/.config` is the user's. It is marked experimental: "run in plan mode first", expect to roll back. Btrfs snapshots,
  migrations and packages make rollback real.
* **Local models are a menu entry:** LM Studio (suggested for newcomers) or Ollama from the AI menu. Nothing routes between them and the cloud agents.
* **One shell process** (Quickshell/QML: bar, launcher, menus, notifications, OSD, lock, polkit) with plugins you *clone* rather than fork. The docs say
  plainly that plugins are arbitrary, unsandboxed code in the long-lived shell and require confirmation showing the source URL.
* **Criticisms:** rolling-release instability; Secure Boot disabled; agents that change the system automatically.

**What it gets right:** the agent is a first-class part of the desktop (key, bar, notifications), not an app; reuse the best existing agents instead of
building one; teach agents the system with a skill; a visible usage/budget panel; rollback underneath.
**What it leaves open:** no sandbox or policy for the agents themselves ("danger mode" alias), no desktop API (agents drive a terminal and dotfiles), no
routing between local and cloud models, no audit, no approval UX, no way for an agent to see or act on windows, and customisation by editing files is
exactly the failure mode its own docs warn about.

## Others

* **AgenticLinux** (Fedora 44 `bootc`): the opposite stance, "the OS is a build artifact": an immutable root, atomic updates and rollback, agents work in
  containers (Docker with GPU), **`llmman`** runs local models as OCI images and picks the right llama.cpp build (CUDA, ROCm, Vulkan) for the GPU found.
  An always-on gateway agent (OpenClaw) handles chores such as enabling updates.
* **Shadowfetch "Mission Control"**: give an agent a task and review its work; nothing is locked in without sign-off.
* **MAGI OS**: local-only, Ollama plus Whisper voice, a custom GTK4 shell; **KDE** plasmoids (KDE Assistant: chat, memory, search, voice, shell tools; AgentDiode:
  live status of coding agents in the tray); **NixOS** agents that manage a system declaratively; **Agent OS** (NixOS) booting into Plasma, GNOME, COSMIC.
* **Windows** (agent identity, workspaces, MXC) and **macOS** (agentic coding in Xcode, a rebuilt Siri) arrive from the other direction.

The pattern across all of them: pre-wire the agents people already use; show agent activity and cost in the shell; give agents a way to reach the
system; make rollback cheap. Few have a safe, structured way for an agent to *act on the desktop*, and nobody has local-plus-cloud routing as part of the OS.

## Where Helix goes further

Helix already has the layer the others lack: **Agent Surface** (observe, semantic act, input, tiered grants, audit), the **agent desk** (a separate
workspace where agents operate real windows), **policy-sandboxed background tasks** with review-on-copy and undo, **overrides instead of dotfile edits**
(`look-overrides.json`, `bind-overrides.json`, app rules: reversible, validated, applied live), the **model gateway** with local GPU engine and
routing, and an **MCP server** onto the desktop. The plan is to make Helix the best *host* for agents (any of them) and the best *substrate* for acting.

### 1. Bring your own agent, first class (Omarchy's best idea, done safely)

* Ship launchers for Claude Code, Codex, OpenCode, Gemini CLI, Copilot CLI, Pi and others as lazy stubs, with a default and a global key that opens it in
  the focused window's folder. Start each inside a Helix **policy sandbox profile** (mxc-style: folder, network, tools) by default; the person can
  widen it per agent ("danger mode" is a profile, with an expiry and a visible banner), never an unaudited alias.
* Every agent gets an **Agent Surface client identity**, the Helix MCP server pre-configured, and a Helix **skill** that teaches it the safe ways to
  change the desktop: Settings and compositor overrides over IPC, never editing `config.kdl` or system files; reading `docs/design`-style capability
  lists. This is the Omarchy skill, with real reversibility behind it.
* Where agents can be told about Helix concepts (agent desk, review-on-copy, grants), they use them.

### 2. Agents in the shell

* A bar **agents panel**: running agents and tasks, state (working, waiting for you, done), tokens and cost by day and model, plan limits and weekly
  budget where the agent reports them, and gateway spend. Aggregated across the person's machines later. Click to open its session or the review.
* **Handoffs from the system**: a crash notification, a failed service, a failed update, a Settings error each offers "Ask an agent" with the log and
  context attached (`diagnose-crash`-style skills for Helix's own services). The summon key already carries the focused window's context.
* Notifications from agents are first-class (waiting for approval, finished, failed) and actionable.

### 3. A rollback story strong enough to allow more autonomy

* **Snapshots**: before an agent task that can change the system or files, take a snapshot where the filesystem supports it (Btrfs/XFS reflink/ZFS), else the
  copy-on-write workspace Helix already uses; one-click restore in the review. (Omarchy relies on Btrfs; Helix makes it part of the task.)
* **Overrides are the safe customisation surface**: everything the Settings pages change can be set, listed and reset by an agent through the same
  validated IPC; "reset to default" always exists. Expand coverage (shortcuts, rules, effects: done) until no common customisation needs a file edit.
* **Time-boxed autonomy grants**: "let this agent do X in this folder for the next hour, without asking": scope, expiry, and a banner; ends on its own;
  everything it did is in the audit and undoable where possible.
* **Graduated autonomy**: per agent, per scope, starting at ask-each-time and moving to plan-approval, then to auto-approve within a sandbox as it
  earns trust (the Anthropic data shows people do this anyway: auto-approve rises from ~20% to over 40% of sessions with experience; build it deliberately and
  with limits instead of leaving it as a hidden "YOLO" flag).

### 3b. An immutable-ish base is optional, not required

AgenticLinux's "OS as a build artifact" is the cleanest rollback, but Helix targets Fedora with user-level installs and an overrides model, so it gets
most of the benefit (agents cannot corrupt the system because they never write system files; they use IPC and sandboxes). Keep `bootc`/image-based
Helix as a later *edition*, not a prerequisite.

### 4. Local models as part of the OS, not a menu entry

Done or in flight: the GPU engine (CUDA and Vulkan), the gateway, routing, the Models app. Add: `llmman`-style **engine selection per GPU** (done in
`build-llama-engine.sh`), model downloads and recommendations for this machine, and the agents themselves able to use the gateway (OpenAI-compatible endpoint,
off by default) so Claude Code, Codex and OpenCode can use a *local* model for cheap turns and route up for hard ones (`model-routing.md`).

### 5. "Lab": a place to test the boundaries

An explicit **Lab** area in Settings for powerful, less-proven capabilities, each off until switched on and each with its own audit and kill switch:
auto-approve scopes, an always-on gateway agent for system chores (updates, cleanup, a "what changed" digest), computer use on the person's own session
(not only the agent desk) with hand-off, agents that edit Helix itself in a staging copy, proactive suggestions, screen-aware context (opt-in, local,
encrypted, never uploaded; the Recall lesson). The point is to move fast on what the owner wants to try without making it the default for everyone.

## Status

* 2026-10-23: `helix-agents` (`services/helix-agents`) starts the *regular* Claude Code, OpenCode and Codex CLIs (nothing is replaced or forked; running
  `claude` yourself still works exactly as before). `helix-agents run <agent> [--profile ask|auto|open|danger] [--dir PATH|focused] [--for 30m] [-- flags]`
  adds, for that run only and without editing any of the agent's own config files: a bubblewrap box (project folder and the agent's own state writable,
  network on, system and tool folders read-only, the rest of the home folder, SSH keys, other programs' logins and the D-Bus session bus absent), the
  Helix desktop tools over an MCP socket bridge (the real adapter runs outside the box; the agent only gets a proxy), a short note about Helix, and
  a launch log. Profiles: `ask` (boxed, the agent's prompts stay), `auto` (boxed, prompts off: the one to live in), `open` (no box), `danger` (no box, no
  prompts, needs `--for`). Also `terminal` (new terminal in the focused window's folder), `default`, `list`, `doctor`, `install-links`
  (`helix-claude`, `helix-opencode`, `helix-codex`, `ha`). 11 unit tests and `tools/agents-test.sh` (a pretend agent: what the box hides, per-profile flags,
  logging, and a real MCP round trip through the proxy to the real adapter). The three real CLIs were started inside the box (`--version`).
  Not yet: the global key and bar entry, installing the links, the agents panel, snapshots.

## Build order (this stance)

1. **Agent launchers and sandbox profiles** (lazy stubs, default agent, global key, per-agent Surface identity, MCP and skill pre-wired).
2. **Agents panel in the bar**: running tasks and agents, state, cost; gateway spend; notifications.
3. **Helix skill and system handoffs** (crash, failed service, settings error → "Ask an agent").
4. **Snapshots in the task flow** and time-boxed autonomy grants; graduated autonomy per agent.
5. **Gateway endpoint for agents** (local model for cheap turns, route up for hard), with the policy router.
6. **Lab** with the first experiments: an always-on chores agent, screen-aware context (opt-in), computer use on the live session with hand-off.
7. Later: aggregate usage across machines; an image-based Helix edition.

## Sources

Omarchy manual (learn.omacom.io, AI chapter), *Omarchy 4 Quattro: What's New* (codetocloud.io), *Omarchy: DHH's distro that became an agentic operating system*
(linuxpro.com.br, Sept 2026), *What Is Omarchy?* (mindstudio.ai), *Announcing AgenticLinux* (ericcurtin.github.io, 18 Sep 2026), MAGI OS and Agent OS repositories,
KDE assistant and AgentDiode projects, NixOS discourse threads, and the Anthropic approval-behaviour figures cited in `ai-usage-and-os-direction.md`.
Some figures (funding, download counts) are from secondary coverage.
