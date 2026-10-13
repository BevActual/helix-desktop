# Helix MCP server

Stdio **MCP** adapter so any MCP-capable app or agent can **see and drive** the live Helix
session through **Agent Surface** (`org.helix.Desktop.AgentSurface1`). This is not Mobius (the human AI hub) and not the
bar Assistant UI.

Design: [`docs/design/inference-fabric.md`](../../docs/design/inference-fabric.md),
[`docs/design/agentic-os.md`](../../docs/design/agentic-os.md).

## Who uses it

Any client that speaks MCP: IDEs, agent runners, automation apps, Helix-adjacent tools.
Each connection is its own Agent Surface identity with separate grants, confirmation, and audit.

Example (Cursor): project [`.cursor/mcp.json`](../../.cursor/mcp.json) points at `run.sh`.
That is one integration, not the definition of this server.

## Setup

```bash
./tools/helix-agent/run.sh
# first run creates .venv and installs requirements.txt
```

Or manually:

```bash
cd tools/helix-agent
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt   # pins mcp<2 (FastMCP API)
```

Requires a running Helix Shell and, for window tools, `niri` on `PATH` with
`NIRI_SOCKET` set in the session.

Optional: `HELIX_SHELL=/path/to/helix-shell` to prefer a specific binary (defaults to
`build-debug/helix` in this repo when present).

### Example: Cursor

1. Reload the Cursor window (or restart MCP servers).
2. Confirm **helix-agent** tools appear (e.g. `desktop_status`, `desktop_capture`).
3. Ask the agent to capture the desktop or toggle Control Center.

## How calls reach the desktop

The adapter prefers the desktop service's Agent Surface D-Bus interface. There, the service applies per-client grants,
asks you before anything in the `confirm` tier (a dialog in the shell), labels private content as untrusted and
writes an audit log. The first call that needs a tier asks you to grant it.

`allowlist.toml` `[backend] mode` selects the path: `auto` (default) uses Agent Surface when the service has it and the
direct `helix-panel` socket otherwise; `surface` requires Agent Surface (`agent1` is a legacy alias); `legacy` never uses it. A refusal from
Agent Surface is final and is never retried on the direct path. The direct path has no per-client grants or
confirmation, so once your install includes Agent Surface, set `mode = "surface"`.

Window and workspace tools (`windows_list`, `workspaces_list`, `window_focus`) call `niri` directly. The service does
not mediate them yet, but they still need the matching grant.

PyGObject comes from the system packages, so `run.sh` creates the virtualenv with system site packages.

## Allowlist

[`allowlist.toml`](allowlist.toml) gates which tools are enabled. Dangerous session power
actions and synthetic input are out of scope for v1.

## Tools

| Tool | Action |
|------|--------|
| `desktop_status` | Shell status JSON |
| `desktop_capture` | PNG path via agent socket (`desktop-capture`; `helix-panel desktop-capture` for scripts) |
| `desktop_context` | Status + focused niri window |
| `panel_toggle` / `panel_open` / `panel_close` | Panels (CC/NC via context) |
| `settings_open` | Helix Settings (`helix-settings`) |
| `bar_visibility` | show / hide / toggle |
| `clipboard_get` / `clipboard_set` | Clipboard |
| `notify` | Notification (history + bar badge) |
| `notifications_recent` | Recent history JSON |
| `workspace_step` | next / prev workspace |
| `workspaces_list` | niri workspaces JSON |
| `windows_list` / `window_focus` | niri windows |
| `app_launch` | Launch an installed app by desktop id or name |
| `window_close` | Close the focused window, or a window id |
| `dnd_toggle` | Toggle Do not disturb; returns on or off |
