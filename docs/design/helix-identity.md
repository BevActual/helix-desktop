# Helix identity

The product is **Helix**. Install names, IPC, UI strings, config paths, and docs use Helix only.

## Repository

| Role | Name |
|------|------|
| Git monorepo (this tree) | **`helix-desktop`** — compositor, greeter, session shell, apps, services, protocols, legacy C++ core |
| Former clone URL | `helix-shell` (renamed; GitHub redirects old links) |

**Helix Shell** is not the repo name: it is the **session chrome** layer (bar, Control Center, launcher, notifications, …) implemented by `helix-shell-tauri`. Do not confuse the repo with the Rust crate **`helix-desktop`** in `services/helix-desktop`, which is only the future desktop-authority process (`org.helix.Desktop`).

## Live names

| Role | Name |
|------|------|
| Session shell binary | `helix-shell-tauri` (`tools/switch-shell.sh install`) |
| Panel helper | `~/.local/bin/helix-panel` |
| Settings / Software / Monitor | `helix-settings`, `helix-software`, `helix-system-monitor` |
| **Mobius** (desktop AI) | Brand for Talk / Act / Agents — like Siri or Gemini for Helix |
| Mobius — Talk | Bar dropdown (quick ask) + window **Mobius**; binary `helix-mobius`; desktop id `org.helix.Mobius` |
| Mobius — Act | Same bar dropdown (Act mode) + `helix-assistant`; Caps Lock `Ask Mobius:` rows |
| Mobius — Agents | Same bar dropdown (Agents mode) + Settings **Privacy & agents** (grants) |
| Desktop service (live) | `helix-service` / `org.helix.Desktop` |
| Desktop service (Rust target) | `helix-desktop` / same bus name (`services/helix-desktop`) |
| Assistant / inference | `helix-assistant`, `helix-inference` (`services/`; Rust target — interim C++ in `cpp/inference/`) |
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
  Tree: `apps/helix-mobius/`.
