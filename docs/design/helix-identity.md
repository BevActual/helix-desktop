# Helix identity

The product is **Helix**. Install names, IPC, UI strings, config paths, and docs use Helix only.

## Live names

| Role | Name |
|------|------|
| Session shell binary | `helix-shell-tauri` (`tools/switch-shell.sh install`) |
| Panel helper | `~/.local/bin/helix-panel` |
| Settings / Software / Monitor | `helix-settings`, `helix-software`, `helix-system-monitor` |
| AI hub (Mobius) | Display **Mobius**; binary `helix-mobius` (transition: `helix-chat`); desktop id `org.helix.Mobius` |
| Desktop service (live) | `helix-service` / `org.helix.Desktop` |
| Desktop service (Rust target) | `helix-desktop` / same bus name (`services/helix-desktop`) |
| Assistant / inference | `helix-assistant`, `helix-inference` (live under `src/`; target `services/`) |
| Helix MCP server | `tools/helix-agent` (stdio MCP for any MCP-capable app/agent) |
| Agent Surface (desktop change) | D-Bus `org.helix.Desktop.AgentSurface1` + `AgentSurfaceActivity1` on `org.helix.Desktop`; see `agent-and-inference.md` |
| Config / state | `~/.config/helix/`, `~/.local/state/helix/` |
| Mobius transcripts | `~/.local/share/helix/mobius/` (migrate from `…/helix/chats`) |
| Inference routing | `~/.local/state/helix/inference-routing.toml` |
| Command socket | `$XDG_RUNTIME_DIR/helix-<WAYLAND_DISPLAY>.sock` |
| Layer namespaces | `helix-panel`, `helix-wallpaper`, … |

Product layout: `product-structure.md`. Agentic fabric: `inference-fabric.md`.

## Retired (removed)

- C++ session UI (`src/shell/`, `helix-shell` binary)
- GTK Settings / Software / Monitor (`src/apps/`) — live Settings is `apps/helix-settings`

## Renamed (transition)

- **Helix Chat** / `helix-chat` / `org.helix.Chat` → **Mobius** / `helix-mobius` / `org.helix.Mobius`.
  Tree may stay `apps/helix-chat/` until the path rename; product strings and desktop id use Mobius.
