# Helix product structure

How Helix is shaped as one product, and how the pieces are supposed to run.
Day-to-day paths: `HELIX.md`. Session install: `session-shell.md`. Names: `helix-identity.md`.
Desktop bus cutover: `cpp-to-rust.md`. Contracts: `protocols/`.

## One product, three session stages

```
greetd → helix-greeter → helix-niri (compositor) + Helix Shell (+ apps)
```

| Stage | Tree | Process role |
|---|---|---|
| Login | `greeter/` | Authenticates; starts the session (greetd) |
| Compositor | `compositor/` | Wayland display server (niri fork; own Cargo workspace) |
| Shell session | `shell/`, `apps/`, `services/`, `core/` | Panels, Control Center, Settings, desktop authority |

Fedora + helix-niri are the sole shell target. Everything else is a helper or contract around that session.

## Runtime map

```
┌─────────────────────────────────────────────────────────────┐
│  compositor (helix-niri)                                    │
│    layer-shell surfaces, outputs, gamma, …                  │
└─────────────┬───────────────────────────────────────────────┘
              │ Wayland + niri IPC
┌─────────────▼───────────────────────────────────────────────┐
│  helix-shell-tauri                                          │
│    bar, Control Center, confirmer, wallpaper surface        │
└─────────────┬───────────────────────────────────────────────┘
              │ D-Bus org.helix.Desktop  (protocols/)
┌─────────────▼───────────────────────────────────────────────┐
│  Desktop authority                                          │
│    LIVE: C++ helix-service (src/desktop/)                   │
│    TARGET: Rust helix-desktop (services/helix-desktop)      │
│    domains: Configuration, Appearance, Audio, Network,      │
│             Bluetooth, Displays, AgentSurface, Activity     │
└─────┬───────────────┬───────────────┬───────────────────────┘
      │               │               │
      ▼               ▼               ▼
 helix-settings   helix-software   Mobius / …
 (apps/)          (apps/)          (apps/)

Optional later: helix-assistant, helix-inference (still C++ under src/)
Session helpers: helix-wallpaper, helix-idle (services/)
```

**One bus owner.** Never run C++ and Rust `org.helix.Desktop` together. The installed `helix-service-launch` picks Rust `helix-desktop` by default and the C++ `helix-service` as fallback (`HELIX_SERVICE=cpp` forces C++; nested harnesses use `HELIX_SERVICE_BIN`).

## Tree roles (intended)

| Path | Owns | Must not own |
|---|---|---|
| `protocols/` | D-Bus XML + IPC schemas (source of truth) | Implementation |
| `core/helix-shell-core` | Pure models, risk policy, config catalog helpers | D-Bus servers, UI |
| `core/helix-desktop-api` | Typed client for `org.helix.Desktop` | Service logic |
| `core/shared-ui` | Shared CSS tokens, icons, and `react/` (`@helix/ui` components, generated TS bindings) | Session policy |
| `shell/` | Live session UI (layer-shell) | Device backends |
| `apps/` | Standalone Helix apps (Settings, …): Tauri host + React/TypeScript frontend (`frontend-stack.md`) | Claiming the Desktop bus |
| `services/helix-desktop` | Rust desktop authority (migration target) | Compositor |
| `services/helix-wallpaper`, `helix-idle` | Small session helpers | Desktop bus name |
| `greeter/` | Login only | Session shell surfaces |
| `compositor/` | Display server (separate workspace) | Shell chrome |
| `src/desktop/` | Live C++ `helix-service` until cutover | New UI |
| `src/` (platform) | C++ config/theme/dbus/… migrating domain-by-domain | New session UI |
| `tools/` | Install, nested e2e, layering checks | Product runtime |
| `docs/design/` | Architecture + HIG | Generated dumps (`archive/`) |

## How it should function

1. **Contracts first.** Clients and both service implementations follow `protocols/`. Rust catalog in `helix-desktop-api` is checked against that XML.
2. **Shell is presentation.** It registers as desktop (`RegisterDesktop`), shows state from snapshots, and writes through Configuration / domain Actions. It does not own PipeWire/NM/BlueZ long-term—the service does.
3. **Apps are peers.** Settings is an app on the same bus, not a second shell. Manager/agent roles are explicit (see Agent Surface design).
4. **Backends are replaceable behind snapshots.** Unavailable backends return readable empty/unavailable snapshots; pages still navigate.
5. **Migration is domain cutover, not a big bang.** Parity on nested/unit tests, then delete the C++ domain in the same change (`cpp-to-rust.md`).
6. **Visual language is one HIG.** GTK4/libadwaita for shared controls; glass only on floating chrome (`helix-hig.md`).
7. **Apps and the shell are Tauri + React + TypeScript + Vite** over typed commands (the shell moves in slices); only the trust surfaces
   (approval dialog, greeter) stay hand-written (`frontend-stack.md`).

## Process inventory (target end state)

| Process | Bus / role | Tree (live → target) |
|---|---|---|
| `helix-greeter` | greetd session | `greeter/` |
| `helix-niri` | compositor | `compositor/` |
| `helix-shell-tauri` | session UI; Desktop registrant | `shell/` |
| `helix-desktop` | `org.helix.Desktop` owner | `services/helix-desktop` (scaffold) ← replaces `src/desktop/` / `helix-service` |
| `helix-settings` etc. | Desktop clients | `apps/` |
| **Mobius** (`helix-mobius`) | AI hub (talk, personas, routing); never Agent Surface grants | `apps/helix-mobius/` |
| `helix-wallpaper`, `helix-idle` | helpers | `services/` |
| `helix-assistant` | Agent Surface client; never in the shell | `services/helix-assistant` (ported from `src/assistant/`) |
| `helix-inference` | `org.helix.Inference` fabric | `src/inference/` → **`services/helix-inference`** |
| Helix MCP server | MCP → Agent Surface for MCP-capable apps/agents | `tools/helix-agent/` |

Rules that stay true after cutover:

- The **shell never holds Agent Surface grants** for the model. Assistant stays a separate process.
- **Mobius never holds Agent Surface grants**; desktop act uses `Shell1.AssistantSubmit`.
- Inference stays sandboxed and D-Bus activated (`assets/services/helix-inference.service.in` today).
- New service binaries land under `services/`; `src/` shrinks until platform C++ is gone or isolated.
- Agentic fabric: `inference-fabric.md`.

## Structure tensions to resolve (active)

| Tension | Today | Target |
|---|---|---|
| Two desktop implementations | C++ live + Rust scaffold | Rust owns the bus; C++ `src/desktop/` gone |
| Stale narrative in older design pages | Entry + compositor user docs marked; `archive/` ignored | Ignore `archive/`; leave protocol-level “if available” wording where accurate |
| C++ platform blob | `src/config`, `theme`, `dbus`, … | Move with each domain into Rust crates / thin helpers |
| Assistant / inference location | `src/assistant`, `src/inference` | `services/helix-assistant`, `services/helix-inference` after Agent Surface on Rust Desktop |
| Doc drift | Entry docs disagree on `services/` | This file is the structure SoT; others link here |

## Reading order

See `docs/design/README.md` for the full index. Short path:

1. This file — product shape and ownership  
2. `session-shell.md` — live install / restart rules  
3. `cpp-to-rust.md` — cutover checklist  
4. `protocols/` — wire contracts  
5. `helix-hig.md` — UI rules  
6. `inference-fabric.md` — Mobius + routing + MCP layer  
7. `agentic-os.md` / `agent-and-inference.md` — Agent Surface details  

When a design doc and `protocols/` disagree, **fix the doc or the XML in the same change**; do not leave both “true.”
