# Helix Agent Surface (MCP)

Stdio MCP adapter so Cursor (and other MCP clients) can **see and drive** the live Helix
session. This is the v1 Agent Surface client — not Agent Center / Assistant UI.

Design: [`docs/design/agentic-os.md`](../../docs/design/agentic-os.md)

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

Requires a running Helix Shell (`noctalia`) and, for window tools, `niri` on `PATH` with
`NIRI_SOCKET` set in the session.

Optional: `HELIX_NOCTALIA=/path/to/noctalia` to prefer a specific binary (defaults to
`build-debug/noctalia` in this repo when present).

## Cursor

Project config: [`.cursor/mcp.json`](../../.cursor/mcp.json).

1. Reload the Cursor window (or restart MCP servers).
2. Confirm **helix-agent** tools appear (e.g. `desktop_status`, `desktop_capture`).
3. Ask the agent to capture the desktop or toggle Control Center.

## Allowlist

[`allowlist.toml`](allowlist.toml) gates which tools are enabled. Dangerous session power
actions and synthetic input are out of scope for v1.

## Tools

| Tool | Action |
|------|--------|
| `desktop_status` | Shell status JSON |
| `desktop_capture` | PNG path via `noctalia msg desktop-capture` |
| `desktop_context` | Status + focused niri window |
| `panel_toggle` / `panel_open` / `panel_close` | Panels (CC/NC via context) |
| `settings_open` | GTK Settings |
| `bar_visibility` | show / hide / toggle |
| `clipboard_get` / `clipboard_set` | Clipboard |
| `notify` | Notification (history + bar badge) |
| `notifications_recent` | Recent history JSON |
| `workspace_step` | next / prev workspace |
| `workspaces_list` | niri workspaces JSON |
| `windows_list` / `window_focus` | niri windows |
| `app_launch` | Launch an installed app by desktop id or name |
| `window_close` | Close the focused window, or a window id |
| `dnd_toggle` | Toggle Do Not Disturb; returns on or off |
