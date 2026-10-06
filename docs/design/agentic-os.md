# Helix Agentic OS

Helix is becoming an **agentic OS**: one desktop that **humans and machines** share through
the same contracts. Cursor (or any MCP client) is a *brain*; Helix owns eyes, hands, and
session context.

## Target product

| Piece | Role |
|-------|------|
| **Agent Surface** | Session APIs — observe, semantic act, vision; later synthetic input |
| **Agent Center** | Multi-agent hub (Muse / ChatGPT / Gemini style) — conversations, specialized agents, history, routing, grants |
| **Helix Assistant** | Siri-like presence — bar/hotkey/voice; looks at the desktop and acts via the Surface |

Both Hub and Assistant are **clients** of the Agent Surface. Do not ship their UIs before the
Surface can see and act.

## Capability tiers

1. **Observe** — `status`, window list, clipboard read, desktop capture (PNG path)
2. **Act (semantic)** — panel open/toggle, settings, bar visibility, workspace step, focus window, notify, clipboard write
3. **Input (synthetic)** — virtual pointer/keyboard / AT-tree click *(phase 2; not v1)*
4. **Dangerous** — session logout/reboot/shutdown, unrestricted raw IPC *(blocked or confirmed)*

v1 implements tiers 1–2 only. Trust model: same as the user’s graphical session (Unix socket
under `$XDG_RUNTIME_DIR`). Do not expose the Surface over the network.

## v1 surface

### IPC

- Existing: `noctalia msg …` (panels, bar, settings, clipboard, notifications, workspaces, …)
- New: `noctalia msg desktop-capture [output|all]` → `ok <png-path>\n` for agent vision
- Windows: `niri msg` / `NIRI_SOCKET` (list + focus) until first-class Helix wrappers exist

### MCP adapter

`tools/helix-agent/` — stdio MCP wrapping the allowlisted tools below. Wired for Cursor via
`.cursor/mcp.json`. Other runners can speak the same IPC without MCP.

| Tool | Backend |
|------|---------|
| `desktop_status` | `noctalia msg status` |
| `desktop_capture` | `noctalia msg desktop-capture` |
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

`protocols/ipc/msg-risk.toml` assigns every `noctalia msg` command a tier: `observe`, `observe-private`
(returns screen, clipboard or notification content; output is untrusted data), `act`, `confirm`, or `blocked`.
`tests/msg_risk_policy_test.py` fails when a command has no tier, so new commands cannot ship unclassified.
The MCP adapter enforces the policy in one place (`noctalia_msg`), refuses `blocked` and unknown commands
regardless of configuration, and grants the tiers listed in `tools/helix-agent/allowlist.toml`. Model-supplied
text is passed after `--` so it is never parsed as a flag. `noctalia schema-json` dumps the full msg schema
for generated tooling.

**Not yet enforced:** the policy lives in the adapter. The IPC socket itself still accepts every command from
any process in the user session, so a client with a shell bypasses the adapter. The `confirm` tier is granted
only because no shell-side confirmation exists yet. Closing both is the `Agent1` D-Bus interface (peer
identity, per-client grants, confirmation, audit log); see `agent-and-inference.md` for that design and the
built-in inference service.

`notification-show` JSON may set `"history": false` for toast-only; CLI/MCP default is history on so agents can drive the unread badge.

## Human entry

The same verbs are available without an agent client. Raycast, Spotlight, and
Prox-style command surfaces put **actions** next to search; Helix keeps that
list on the existing launcher and Caps Lock insert instead of adding an
assistant window.

- Launcher prefix `act` (also in global search): Notification Center, Control
  Center, Settings, Helix Software, System Monitor, Do Not Disturb, workspace
  step, bar visibility, and close the focused window.
- Caps Lock opens **Actions** first (emoji, screenshots, and clipboard stay one
  Tab away). The row under the search field names the focused window, the same
  context `desktop_context` gives an agent. Settings, Software, and System
  Monitor switch to the open window when that app is already running. Workspace
  rows name the workspace you are on.
- `noctalia msg app-launch <id-or-name>` opens any installed application.
  Settings and Software use the shell's existing app launch path.

## Phased roadmap

1. **v1:** Agent Surface + MCP — capture + semantic desktop control  
2. **v1.1:** CC/NC polish, notify→history/badge, `desktop_context` (focused window)  
3. **Phase 2:** selection / a11y + synthetic input + grants UI  
4. **Phase 3:** Agent Center (hub) + Assistant presence sharing the Surface

## Non-goals (v1)

- Pixel click/type
- Network-exposed agent socket
- Full Agent Center / Assistant UI
- Cross-user or sandboxed third-party agents
