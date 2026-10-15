# Design docs

Canonical product shape: **[product-structure.md](product-structure.md)**.

## Start here

| Doc | When you need it |
|---|---|
| [product-structure.md](product-structure.md) | How Helix is split (greeter / compositor / shell / apps / services) and how processes relate |
| [session-shell.md](session-shell.md) | Live Tauri shell install, restart rules, retired C++ UI |
| [cpp-to-rust.md](cpp-to-rust.md) | History of the C++ → Rust cutover (complete); remaining native shims and greeter items |
| [helix-identity.md](helix-identity.md) | Product names, paths, retired brand leftovers |
| [helix-hig.md](helix-hig.md) | Visual / interaction language (chrome vs leftover vs modules) |
| [rust-workspace.md](rust-workspace.md) | Cargo workspace members and build profiles |
| [frontend-stack.md](frontend-stack.md) | **How app UIs are built**: Tauri + React + TypeScript + Vite; what stays hand-written |

## Contracts and platforms

| Doc | When you need it |
|---|---|
| `../../protocols/` | D-Bus + IPC schemas (source of truth for wire types) |
| [desktop-service.md](desktop-service.md) | Domain API behavior for `org.helix.Desktop` (verify against `protocols/`) |
| [desktop-contract-tests.md](desktop-contract-tests.md) | What bus/integration tests must pass for Tauri + Settings; cleanup backlog |
| [layering.md](layering.md) | Cargo workspace dependency rules and the `layering` check |
| [trusted-surface.md](trusted-surface.md) | Trust boundaries for shell surfaces |

## Surfaces and apps

| Doc | When you need it |
|---|---|
| [tauri-rust-shell.md](tauri-rust-shell.md) | Session shell (Tauri) milestones and nested testing |
| [greeter-tauri.md](greeter-tauri.md) | Login greeter |
| [frontend-stack.md](frontend-stack.md) | App frontend stack, shared components, generated bindings, status per app |
| [settings-tauri.md](settings-tauri.md) | Settings app |
| [software-tauri.md](software-tauri.md) | Software app |
| [system-monitor-tauri.md](system-monitor-tauri.md) | System Monitor |
| [settings-audit.md](settings-audit.md) | Settings coverage audit |

## Agent

| Doc | When you need it |
|---|---|
| [mobius-product.md](mobius-product.md) | Everyday workflows, chat context/brief, and next capability/UX priorities |
| [mobius-analysis.md](mobius-analysis.md) | REA/Ghidra software analysis capability, runner boundaries, setup, and delivery slices |
| [inference-fabric.md](inference-fabric.md) | Mobius hub, task routing, MCP layer, Act-slot isolation |
| [agentic-os.md](agentic-os.md) | Agent Surface / session agent surface |
| [agent-and-inference.md](agent-and-inference.md) | Agent Surface, inference process, eval notes |

## Archive

`archive/` holds retired dumps and old handoffs. Do not treat it as current structure.
