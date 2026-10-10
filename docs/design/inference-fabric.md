# Inference fabric and Mobius hub

Siri-shaped Helix: many presence doors, **Mobius** as the AI hub, bar Assistant as the quick act door,
MCP as a protocol layer for MCP-capable apps and agents, and an Inference fabric that routes tasks to
on-device, LAN, or remote models. Remote brains never hold Agent Surface.

Product structure: `product-structure.md`. Agent Surface: `agentic-os.md`. Agent Surface / inference process:
`agent-and-inference.md`. Names: `helix-identity.md`.

## Pieces

| Piece | Role |
|---|---|
| **Agent Surface** | Desktop hands — Agent Surface → shell `agent_ipc` |
| **Bar Assistant** | Explicit act presence (`helix-assistant`); Caps Lock act rows |
| **Mobius** | Central hub — conversations, personas, model routing, Act handoff (renamed from helix-chat) |
| **MCP adapter** | `tools/helix-agent` — any MCP-capable client onto Agent Surface (not Cursor-only) |
| **Inference fabric** | `org.helix.Inference` — task router + backends |

Do not merge Mobius, the bar Assistant, and the Agents panel into one process.

## Hard rules

1. **One hallway for desktop change.** Agent Surface → shell socket. First-party act ends in `helix-assistant`.
   MCP clients get their own Agent Surface identity and grants. **Mobius never holds Agent Surface client grants**;
   it uses `org.helix.Shell1.AssistantSubmit`.
2. **Only the on-device Act slot may emit tools** for the built-in Assistant (GBNF from `schema-json`).
   Larger / LAN / cloud models are for talk, summarize, vision, code — explicit escalation, visible grant,
   never silent fallback.
3. **Routing is first-class policy**, managed in Mobius and read by Inference.
4. **LAN is not local.** Opt-in, named in Mobius; same privacy stance as cloud. No Agent Surface tools from
   LAN/cloud replies unless a local Act slot re-grounds the plan.
5. **MCP is a protocol layer**, not a Cursor feature. Product name: Helix MCP server.

## Invocation

| Person does | Opens | Then |
|---|---|---|
| Ambiguous hotkey / voice / “ask” | **Mobius** | Active persona + Inference talk |
| Explicit act (Act on desktop, Caps Lock act) | Bar Assistant | Act slot → Agent Surface |
| Manage models / personas | Mobius | Hub UI |
| MCP-capable app connects | MCP adapter | Own Agent Surface client |
| Selection / share (later) | Mobius thread | Act only if chosen |

Ambiguous presence opens Mobius; the bar stays the dedicated act door.

## Task profiles

| Profile | Default backend | May escalate to |
|---|---|---|
| `act` | On-device tool model + grammar | Never remote for tool JSON |
| `chat` | On-device mid model | LAN / cloud if granted |
| `summarize` | Fast local or LAN | — |
| `vision` | Local multimodal or LAN | — |
| `code` | LAN / code model or cloud | — |

On battery / compositor load: prefer on-device `chat`/`act`; refuse or prompt before LAN/cloud when the
power profile is saver.

## Mobius personas

First-class hub-side minds (not Agent Surface grant holders):

| Owns | Does not own |
|---|---|
| Name, system prompt, memory/transcript scope | Agent Surface client grants |
| Default task profile + model route | Direct desktop tool execution |
| “Suggest Act” UX | Confirmer / SetGrant |

Desktop change: persona turn → **Act on desktop** → `AssistantSubmit` → bar → `helix-assistant` → Agent Surface.

## Trust and data

| Concern | Home |
|---|---|
| Tool risk | `protocols/ipc/msg-risk.toml` + Agent Surface |
| Grants / audit | Desktop service `agent/` state |
| Routing config | Mobius-edited; Inference-read (`~/.local/state/helix/inference-routing.toml`) |
| Transcripts | Mobius data dir (migrate from `~/.local/share/helix/chats`) |
| Local weights | `~/.local/share/helix/models/` |
| LAN/cloud secrets | Secret service; never in transcripts |
| MCP allowlist | `tools/helix-agent/allowlist.toml` |

Mobius may read AgentSurfaceActivity1 for display; never SetGrant / RegisterConfirmer.

## Tree

| Path | Role |
|---|---|
| `apps/helix-chat/` → Mobius (`org.helix.Mobius`, command `helix-mobius`) | Hub app |
| `src/inference/` → `services/helix-inference` | Fabric |
| `src/assistant/` → `services/helix-assistant` | Act client |
| `tools/helix-agent/` | Helix MCP server |
| `protocols/dbus/org.helix.Inference.xml` | Wire contract |

## Non-goals

- Remote / LAN emission of Agent Surface tool JSON
- Network-exposed Agent Surface
- Silent cloud fallback
- Bar as a second conversation hub
- Mobius holding Agent Surface grants
