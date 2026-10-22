# Agents: what people actually do with them

Status: direction, 2026-10-22. It comes from the person's observation that *Act* (the desktop assistant that changes a volume or a theme) is not what
agents are for. People use Hermes, Cursor, Claude and ChatGPT to **get work done**: write and change code, research something and write it up, draft
and edit documents, work through files, operate a browser or an application for them, do something on a schedule, and carry a task over several steps
while they do something else. Helix should be good at *that*, and treat desktop settings as one small tool among many.

## What people ask an agent to do, and where Helix stands

| The task | In practice | Helix today | The gap |
|---|---|---|---|
| **Build or change software** | "Add X to this repo", "fix this failing test", review a diff | Mobius Code stage, reviewed edits with restore points, a bubblewrap terminal, **ACP**: Hermes and OpenCode run as agents inside Mobius (`mobius-acp.md`) | the person's own agent subscriptions are used as they are; polish of the run view |
| **Research and write up** | "Find out about X and give me a report with sources" | Mobius Tools mode: search, page read, citations; Write stage | long runs, saved as a *task* with sources, resumable |
| **Documents and files** | summarise, rewrite, convert, fill in a template | Write stage, attachments, knowledge store, Save as file | agent edits in place with a visible diff, across many files |
| **Operate a browser or an app** | "Book this", "fill in this form", "download my statements" | **the agent display** (new): its own screen, watch, take over, ask for help; nine `computer_*` tools over MCP | a browser on the display; the planner/grounder split; cloud models |
| **Run commands** | build, test, scrape, convert media | sandboxed terminal (no network, no home), approvals | network allow-lists per task; long-running jobs |
| **Recurring or background work** | "Every morning check X" | Mobius scheduled tasks (no tools, never cloud) | tools and cloud for scheduled agents, with a visible digest |
| **Reach me elsewhere** | a message when it is done or stuck | island and notifications | a phone gateway (later) |
| **Change this desktop** | volume, theme, open an app | *Act* and the shell's command socket (`agentic-os.md`) | none needed; it stays as one tool |

## The reframe

* **An agent task is the unit**, not Act: a goal, a *place to work* (a project folder, a document, or the agent display), the tools it may use, the
  model route it may use (local, LAN or cloud: `computer-use.md`), and a visible run: steps, files changed with diffs, anything waiting for the person.
  The person can start one, watch it, stop it, take over, and come back to its result.
* **Act becomes a tool called "Desktop settings"** that an agent may be granted, with the same tiers and confirmations it has today. It is no longer a
  place of its own in the product, and no longer the reason a setting such as "Act never uses cloud" exists: the model route belongs to the task.
* **Tools are providers behind one gate.** Files (reviewed edits), terminal (sandbox), web (search and read), the agent display (computer use), the
  desktop (Agent Surface) and any MCP server the person adds. Agent Surface stays the one place that decides grants, confirmation and the audit.
* **Bring your own agent.** Helix is a good host rather than a replacement: Hermes, OpenCode and other ACP agents run inside Mobius with Helix's
  reviewed edits and sandbox; any MCP agent (Claude Code, Cursor, others) can use Helix's tools, including the agent display, through
  `tools/helix-agent` (`computer_*`, `app_launch`, `desktop_capture`, and so on). Helix's own loop is for when no outside agent is wanted, and it can
  use cloud models like any of them.

## What to build next, in order

1. **The task as a first-class thing in Mobius** (an "Agents" home): *New task* with a goal, a place and a model policy; a run view with steps, diffs and
   the agent display embedded; Stop, take over, hand back; results saved and resumable. (Tools mode and the Code stage are its first two kinds of run.)
2. **One tool-calling loop with a model router** that can use local, LAN or cloud models, with the tool providers above. This is also where a cloud
   model gets to use the agent display (planner and grounder), and it retires the special local-only Act loop for everything except the settings tool.
   *Built (first slice):* Mobius's Tools-mode loop already routes to local, LAN or cloud models; it now has seven `display_*` tools (launch, look, click,
   type, key, scroll, help) that go through the desktop's Agent Surface like any other client (`surface.rs`: Hello, RequestAccess, Action), so tiers,
   confirmation and audit stay in the shell. The planner can be a cloud model; `display_look` is described by the **local** vision model and only that
   text goes to the planner (pictures never leave the computer), and with a remote planner each look is reviewed first. Opening an app is reviewed;
   after anything on the display the person's later web requests are reviewed too; the step budget is 40 once a display tool is used. Still to do:
   *Also built:* `core/helix-task`: a task (goal, state, steps with where each ran, notes) saved as JSON under `~/.local/share/helix/mobius/tasks`, and a
   **policy** (folder, allowed command prefixes, automatic edits, web, cloud use, display and apps, step and time limits) that decides allow, ask or
   deny for each action; Tools mode now runs as a task under the "ask me first" policy, so nothing changes for the person yet but every run is recorded.
   A chat's Tools panel now offers **Ask first** or **Work in the folder** (the balanced policy: checks such as tests and `git status`, and edits inside the
   folder, run without asking).
   **Background tasks:** the Tasks sheet starts a task (goal, folder, how much to ask) that runs on its own (up to three at once) under the same loop and
   policy; a question to the person shows in the sheet with Allow/Decline and a notification, the answer lands in a new chat with every step, and Stop works
   at any point.
   **Policy-driven sandbox** (after Microsoft's Execution Containers, `microsoft/mxc`, MIT): the terminal's `bwrap` arguments are now built from the task's
   policy (`helix_task::bwrap_args`): the folder is the only writable place, `protect` paths (`.git`) are read-only on top of it, `hide` paths (`.env`,
   `.ssh`) are empty, extra `read_only` paths must be absolute and not the machine, and the network is off unless the person turned it on for the task
   (all or nothing: the sandbox cannot filter by host). A **practice run** lets what would have asked go ahead inside the sandbox, records it on the step,
   and "Run for real" starts again with exactly those commands, edits and apps allowed (never `sudo`, `rm`, `curl`, pipes or paths outside the folder).
   `helix_task::to_mxc` writes a policy as an MXC `1.0.0` configuration (paths, network default, command, time limit) and `read_only_and_network` reads an
   incoming one as far as Helix can honour it (never a wider writable area). MXC's schema is young (alpha versions precede 1.0.0; third-party notes call the SDK
   "stable enough for non-security-critical production"), so Helix's own `bwrap` builder stays the enforcer for now.
   Open question: adopt the `mxc-sdk` crate as the backend for host-filtered networking (proxy) and macOS/Windows later, once it is stable.
   `tools/nested/ui-suite.sh mobius-agent-tasks` drives it end to end with the scripted planner: Allow in the sheet, the folder policy running a check without
   asking, a practice run recording what it would have asked, and Stop while waiting.
   **Review on a copy** (the Codex/Cursor pattern): a background task on a git project works in its own `git clone` of the last commit, kept outside Mobius's data
   folder (so git works inside the sandbox and two tasks cannot collide). When it finishes the sheet offers *Review changes*: the diff against the starting
   commit (committed, uncommitted and new files), then *Apply to project* (a `git apply` that is checked first, so all of it or none, left unstaged) or
   *Discard*. Uncommitted changes in the project are not in the copy and the sheet says so. Non-git folders still work in place.
   **Agents home:** the sidebar's Agents row opens a page of its own (not a dialog): tasks on the left grouped *Needs you*, *Working*, *Ready to review*, *Earlier*,
   with Allow, Decline and Stop on the row itself and a count badge on the sidebar row; on the right the selected task: its answer, the changes to apply or
   discard (the diff opens by itself), and every step with a badge for where it ran. New task is a composer on the same page.
   **Progressive delegation** (the consensus pattern: start asking, widen as trust builds): a waiting command, edit or app launch has *Always allow*, which adds exactly
   that (the first two words of a plain command, edits inside the folder, that app) to the task's policy and to a per-folder memory
   (`~/.local/share/helix/mobius/folder-policy.json`) so the next task in that folder starts with it. Risky or chained commands are never remembered.
   Long step lists show the latest five with the earlier ones one click away.
   Still to do: plan-first (an editable plan to approve before the task acts), a Private/Balanced/Best policy with a cloud badge, taint confirmation, and a nested test driving the loop against the display.
3. **A browser on the agent display** (installed browser launched onto it, its own profile) with the take-over and sign-in handoff, because most "operate
   it for me" tasks are web tasks.
4. **Memory and skills the agent writes for itself**, local and editable (the knowledge store), after the Hermes pattern.
5. **Recurring tasks with tools**, and a gateway to the phone.

Naming follows: in the product, say *tasks* and *agents*; "Act" stops being a heading.
