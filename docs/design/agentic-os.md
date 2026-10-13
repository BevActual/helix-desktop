# Helix Agentic OS

Helix is becoming an **agentic OS**: one desktop that **humans and machines** share through
the same contracts. MCP-capable apps and agents are *brains* on the Surface; Helix owns eyes,
hands, and session context. Fabric (Mobius, routing, MCP layer): `inference-fabric.md`.

## Target product

| Piece | Role |
|-------|------|
| **Agent Surface** | Session APIs — observe, semantic act, vision; later synthetic input |
| **Mobius** | AI hub — conversations, personas, model routing, Act handoff (was “Agent Center” / helix-chat) |
| **Helix Assistant** | Explicit act presence — bar / Caps Lock act; never the long-form hub |
| **Helix MCP server** | `tools/helix-agent` — MCP protocol onto Agent Surface for any MCP-capable client |

Mobius and the bar Assistant are **not** Agent Surface grant holders for the model path Mobius uses for talk;
desktop act goes through `helix-assistant` or MCP clients with their own grants. See `inference-fabric.md`.

## Capability tiers

1. **Observe** — `status`, window list, clipboard read, desktop capture (PNG path)
2. **Act (semantic)** — panel open/toggle, settings, bar visibility, workspace step, focus window, notify, clipboard write
3. **Input (synthetic)** — virtual pointer/keyboard / AT-tree click *(phase 2; not v1)*
4. **Dangerous** — session logout/reboot/shutdown, unrestricted raw IPC *(blocked or confirmed)*

v1 implements tiers 1–2 only. Trust model: same as the user’s graphical session (Unix socket
under `$XDG_RUNTIME_DIR`). Do not expose the Surface over the network.

## v1 surface

### IPC

- Existing: `helix-panel …` (panels, bar, settings, clipboard, notifications, workspaces, …)
- **Administrator password:** `SUDO_ASKPASS=~/.local/bin/helix-askpass` (installed by `tools/switch-shell.sh`) opens a Helix overlay; `sudo -A` and `tools/install-inference.sh` use it when not root.
- New: `helix-panel desktop-capture [agent-desk|output|all]` → `ok <png-path>\n` for agent vision (empty target captures the agent desk when placement is on and helix-niri reports agent-desk support; otherwise the focused output)
- Windows: `niri msg` / `NIRI_SOCKET` (list + focus) until first-class Helix wrappers exist

### MCP adapter (Helix MCP server)

`tools/helix-agent/` — stdio MCP wrapping the allowlisted tools below for **any MCP-capable app or
agent**. Cursor may use it via `.cursor/mcp.json` as one consumer; it is not the product definition.

| Tool | Backend |
|------|---------|
| `desktop_status` | `helix-panel status` |
| `desktop_capture` | `helix-panel desktop-capture` |
| `desktop_context` | status JSON + focused niri window |
| `panel_toggle` / `panel_open` / `panel_close` | `panel-*` |
| `settings_open` | `settings-open` |
| `bar_visibility` | `bar-show` / `bar-hide` / `bar-toggle` |
| `clipboard_get` / `clipboard_set` | `clipboard-text` / `clipboard-copy` |
| `notify` | `notification-show` (history + bar badge by default) |
| `notifications_recent` | `notification-history` |
| `workspace_step` | `workspace-switch next\|prev` |
| `workspaces_list` | `niri msg workspaces` |
| `windows_list` | `niri msg windows` |
| `window_focus` | niri `FocusWindow` |
| `app_launch` | `app-launch` (desktop id or name) |
| `window_close` | `window-close` (focused window, or a compositor id) |
| `dnd_toggle` | `notification-dnd-toggle` |

**Blocked in v1:** synthetic input, `session reboot/shutdown/logout`, `firewall-panic-toggle`, unrestricted raw msg.

### Risk tiers

`protocols/ipc/msg-risk.toml` assigns every `helix-panel` command a tier: `observe`, `observe-private`
(returns screen, clipboard or notification content; output is untrusted data), `act`, `confirm`, or `blocked`.
`tests/msg_risk_policy_test.py` fails when a command has no tier, so new commands cannot ship unclassified.
The MCP adapter enforces the policy in one place (`helix_msg`), refuses `blocked` and unknown commands
regardless of configuration, and grants the tiers listed in `tools/helix-agent/allowlist.toml`. Model-supplied
text is passed after `--` so it is never parsed as a flag. `helix schema-json` dumps the full msg schema
for generated tooling.

**Not yet enforced:** the policy lives in the adapter. The IPC socket itself still accepts every command from
any process in the user session, so a client with a shell bypasses the adapter. The `confirm` tier is granted
only because no shell-side confirmation exists yet. Closing both is the `Agent Surface` D-Bus interface (peer
identity, per-client grants, confirmation, audit log); see `agent-and-inference.md` for that design and the
built-in inference service.

`notification-show` JSON may set `"history": false` for toast-only; CLI/MCP default is history on so agents can drive the unread badge.

## Interconnected surfaces (ships today)

**Mobius** = Helix’s desktop AI. Talk · Act · Agents are doors under that brand;
processes stay separate (see `inference-fabric.md`).

| Surface | Talk | Act | Oversight |
|---|---|---|---|
| **Mobius** (window) | Inference1 | `org.helix.Shell1.AssistantSubmit` → bar → `helix-assistant` → Agent Surface | Hub → Act / Agents; never holds Agent Surface |
| **Bar Mobius** (Talk) | Inference1 (quick) | — | Open full Mobius for long chat |
| **Bar Mobius** (Act) | Switch to Talk door | Agent Surface via `helix-assistant` (shell never holds grants) | Island + confirmer; Agents door |
| **Bar Mobius** (Agents) | Switch to Talk door | Act door; Stop / revoke / kill switch | AgentSurfaceActivity1 |
| **Settings → Privacy & agents** | — | Manager `SetGrant` | Full audit |
| **Helix MCP server** | — | Agent Surface (per MCP client) | Same confirmer / audit |

Ambiguous ask → Mobius Talk; explicit desktop change → Mobius Act. Fabric: `inference-fabric.md`.

## Human entry

The same verbs are available without an agent client. Raycast, Spotlight, and
Prox-style command surfaces put **actions** next to search; Helix keeps that
list on the existing launcher and Caps Lock insert instead of adding an
assistant window.

- Launcher prefix `act` (also in global search): agent desk, panels (launcher,
  assistant, agents, notifications, control center), Settings, Privacy & agents,
  Mobius, Helix Software, System Monitor, Do Not Disturb, Night Light, dark
  theme, lock screen, bar show/hide/toggle, workspace step, and close the focused
  window.
- Caps Lock opens **Actions** first (emoji, screenshots, and clipboard stay one
  Tab away). The row under the search field names the focused window, the same
  context `desktop_context` gives an agent. Settings, Software, and System
  Monitor switch to the open window when that app is already running. Workspace
  rows name the workspace you are on.
- `helix-panel app-launch <id-or-name>` opens any installed application.
  Settings and Software use the shell's existing app launch path.

## Agent desk (helix-niri)

A **named workspace** (default `Helix Agent`, `helix { agent-desk-name "…" }` in niri config) holds
windows opened on behalf of agents. The shell queues placement with `HelixPlaceOnAgentDesk` before
`app-launch` (and the same path Act uses through Agent Surface): new toplevels land on the agent desk
**without stealing focus**. The bar workspace strip marks that pill; click still switches desks
explicitly. `niri msg` request `HelixAgentDesk` reports id, window count, and pending placements.
The bar island shows a glance when the agent desk has windows and you are on another workspace (tap to
focus the desk). **Caps Lock → Actions** includes **Show agent desk**; Mobius shows an agent-desk meta chip when Tools is on or the desk has windows. `desktop-capture` defaults to the agent desk for granted agents. Mobius Tools opens a visible terminal on the agent desk (foot, Ptyxis, …) on the first approved project command when placement is on; sandboxed command output still streams in the thread. `all` and named outputs use `HelixScreenshotAllOutputs` / `HelixScreenshotOutput` on helix-niri.

**Settings integration:** Privacy & agents edits `~/.config/niri/helix-setup.kdl` (and ensures
`include "helix-setup.kdl"` in `config.kdl`), reloads the compositor, and stores session placement
policy in `~/.config/helix/compositor-prefs.toml`. Theme, layout, blur, and outputs stay in their
existing Helix-managed includes (Control Center); key binds remain read-only in Settings → Keyboard.

## Phased roadmap

1. **v1:** Agent Surface + MCP — capture + semantic desktop control  
2. **v1.1:** CC/NC polish, notify→history/badge, `desktop_context` (focused window)  
3. **Phase 2:** selection / a11y + synthetic input + grants UI  
4. **Phase 3:** Mobius hub personas + Inference fabric routing (see `inference-fabric.md`)

## Non-goals (v1)

- Pixel click/type
- Network-exposed agent socket
- Remote emission of Agent Surface tool JSON
- Cross-user or sandboxed third-party agents without Agent Surface grants
