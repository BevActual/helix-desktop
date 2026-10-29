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
| Compositor | `compositor/` | Wayland display server (niri fork; crates in the root Cargo workspace) |
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
│  helix-desktop (services/helix-desktop)                     │
│    org.helix.Desktop — Configuration, Appearance, Audio,    │
│    Network, Bluetooth, Displays, AgentSurface, Activity     │
└─────┬───────────────┬───────────────┬───────────────────────┘
      │               │               │
      ▼               ▼               ▼
 helix-settings   helix-software   Mobius / …
 (apps/)          (apps/)          (apps/)

helix-assistant, helix-inference, helix-system-upgrade (services/)
Session helpers: helix-wallpaper, helix-idle, helix-nightlight (services/)
```

**One bus owner.** The user unit `helix-service.service` (name kept) runs `helix-desktop` on the session bus. Nested harnesses set `HELIX_SERVICE_BIN` to an explicit binary when they need a non-installed build.

## Tree roles (intended)

| Path | Owns | Must not own |
|---|---|---|
| `protocols/` | D-Bus XML + IPC schemas (source of truth) | Implementation |
| `core/helix-base`, `helix-agent`, `helix-config`, `helix-config-export`, `helix-models`, `helix-compositor`, `helix-shell-model`, `helix-chat`, `helix-software-model`, `helix-monitor-model`, `helix-wallpaper-model` | Pure models, risk policy, config catalog (`shell-core-split.md`) | D-Bus servers, UI |
| `core/helix-desktop-api` | Typed client for `org.helix.Desktop` | Service logic |
| `core/shared-ui` | Shared CSS tokens, icons, and `react/` (`@helix/ui` components, generated TS bindings) | Session policy |
| `shell/` | Live session UI (layer-shell) | Device backends |
| `apps/` | Standalone Helix apps (Settings, …): Tauri host + React/TypeScript frontend (`frontend-stack.md`) | Claiming the Desktop bus |
| `services/helix-desktop` | `org.helix.Desktop` authority | Compositor |
| `services/helix-assistant`, `helix-inference`, `helix-system-upgrade` | Agent client, inference fabric, pkexec upgrade helper | Desktop bus name (assistant/inference use their own names) |
| `services/helix-wallpaper`, `helix-idle`, `helix-nightlight` | Small session helpers | Desktop bus name |
| `greeter/` | Login only | Session shell surfaces |
| `compositor/` | Display server (niri fork) | Shell chrome |
| `services/helix-inference/native/` | Interim llama + whisper shims for Cargo bins | Session UI |
| `services/helix-image/native/` | C stable-diffusion shim for the Cargo image worker | Session UI |
| `tools/` | Install, nested e2e, layering checks | Product runtime |
| `docs/design/` | Architecture + HIG | Generated dumps (`archive/`) |

## Build ownership (how it works)

| System | Builds | Install / run |
|---|---|---|
| **Cargo** (workspace root) | Everything Helix: session services, Tauri apps and shell, greeter, `helix-inference` + `helix-transcribe` + `helix-image`, theme, config export | `just build`, `just install`, `tools/switch-shell.sh install`, `tools/install-inference.sh` |
| **npm** (workspace root) | The React frontends, run by each app's `build.rs` | with Cargo |
| **compositor/** | Own Cargo workspace (niri fork) | `tools/install-trusted-niri.sh` |

There is no Meson or CMake step. `tools/install.sh` installs the services, units, policy and assets (with `PREFIX` and `DESTDIR`);
`tools/test-integration.sh` runs the integration tests against `target/fast` or `target/debug`.

Rule: new binaries and tests land in Cargo. Dependency direction between crates is checked by `tools/check-layers.py` (`layering.md`).

## How it should function

1. **Contracts first.** Clients and both service implementations follow `protocols/`. Rust catalog in `helix-desktop-api` is checked against that XML.
2. **Shell is presentation.** It registers as desktop (`RegisterDesktop`), shows state from snapshots, and writes through Configuration / domain Actions. It does not own PipeWire/NM/BlueZ long-term—the service does.
3. **Apps are peers.** Settings is an app on the same bus, not a second shell. Manager/agent roles are explicit (see Agent Surface design).
4. **Backends are replaceable behind snapshots.** Unavailable backends return readable empty/unavailable snapshots; pages still navigate.
5. **Change a domain with its contract.** Behavior changes land with the `protocols/` entry and the contract test in `services/helix-desktop/tests/contract/` (`desktop-contract-tests.md`). The C++ → Rust cutover that set this rule is finished (`cpp-to-rust.md`).
6. **Visual language is one HIG.** Shared tokens and `@helix/ui` components; glass only on floating chrome (`helix-hig.md`).
7. **Apps and the shell are Tauri + React + TypeScript + Vite** over typed commands (the shell moves in slices); only the trust surfaces
   (approval dialog, greeter) stay hand-written (`frontend-stack.md`).

## Process inventory (target end state)

| Process | Bus / role | Tree (live → target) |
|---|---|---|
| `helix-greeter` | greetd session | `greeter/` |
| `helix-niri` | compositor | `compositor/` |
| `helix-shell-tauri` | session UI; Desktop registrant | `shell/` |
| `helix-desktop` | `org.helix.Desktop` owner | `services/helix-desktop` |
| `helix-settings` etc. | Desktop clients | `apps/` |
| **Mobius** (`helix-mobius`) | AI hub (talk, personas, routing); never Agent Surface grants | `apps/helix-mobius/` |
| `helix-wallpaper`, `helix-idle`, `helix-nightlight` | helpers | `services/` |
| `helix-assistant` | Agent Surface client; never in the shell | `services/helix-assistant` |
| `helix-inference` | `org.helix.Inference` fabric | `services/helix-inference` (+ `helix-transcribe` bin; llama glue under `native/`) |
| `helix-system-upgrade` | polkit-gated `dnf` plan helper | `services/helix-system-upgrade` |
| Helix MCP server | MCP → Agent Surface for MCP-capable apps/agents | `tools/helix-agent/` |

Rules that stay true after cutover:

- The **shell never holds Agent Surface grants** for the model. Assistant stays a separate process.
- **Mobius never holds Agent Surface grants**; desktop act uses `Shell1.AssistantSubmit`.
- Inference stays sandboxed and D-Bus activated (`assets/services/helix-inference.service.in` today).
- New service binaries land under `services/`. Platform `cpp/` is gone; the remaining C++ is the llama and image engine shims under `services/*/native/`.
- Agentic fabric: `inference-fabric.md`.

## Structure tensions to resolve (active)

| Tension | Today | Target |
|---|---|---|
| Theme | `core/helix-theme`, `services/helix-desktop` | Legacy C++ theme tree removed |
| Inference workers | Rust `helix-transcribe` and `helix-image` crates; llama and image engine shims under `services/*/native/` | Replace those shims with Rust |
| Agent policy C++ | **Deleted** | `helix-desktop-api::agent` |
| Greeter | Tauri greeter with appearance sync | Power actions, last account/session, packaging (`greeter-tauri.md`) |
| Shell frontend | React islands over the hand-written `shell.js` page, with fallbacks | Every slice React; fallbacks deleted (`frontend-stack.md`) |
| Doc drift | Soak + a few entry docs may still mention the C++ desktop | This file is the structure SoT; others link here |

## Reading order

See `docs/design/README.md` for the full index. Short path:

1. This file — product shape and ownership  
2. `session-shell.md` — live install / restart rules  
3. `cpp-to-rust.md` — how the cutover was done (history)  
4. `protocols/` — wire contracts  
5. `helix-hig.md` — UI rules  
6. `inference-fabric.md` — Mobius + routing + MCP layer  
7. `agentic-os.md` / `agent-and-inference.md` — Agent Surface details  

When a design doc and `protocols/` disagree, **fix the doc or the XML in the same change**; do not leave both “true.”
