# C++ → Rust migration

Helix’s session UI is already Rust (Tauri). Remaining C++ owns desktop authority and helpers. This doc is the cutover map.

## Inventory

| C++ today | Rust home | Status |
|---|---|---|
| `helix-service` | `services/helix-desktop` (bin `helix-desktop`) | Scaffold; C++ still owns the live bus name |
| `src/desktop/agent_*`, `tool_schema` | `helix-shell-core::agent` (+ service) | Risk policy, GrantStore, peer identity, Agent Surface Hello/Action/Confirm/manager |
| `src/config/`, theme, dbus adapters | service / shared crates | Port with each domain |
| `helix-assistant` | `services/helix-assistant` | After Agent Surface on Rust Desktop; keep separate from shell |
| `helix-inference` | `services/helix-inference` | Last; llama.cpp via FFI; sandboxed D-Bus activation |
| `helix-system-upgrade` | small helper or stay C++ | Low priority |
| `compositor/` | already Rust (own workspace) | Done |
| `greeter/` | Tauri scaffold | In progress |

## Cutover rules

1. **`protocols/` is authoritative.** D-Bus XML and IPC schemas stay the single source of truth. Rust checks or generates against them.
2. **One bus owner.** Never run two `org.helix.Desktop` processes. Nested harness / install picks C++ or Rust via `HELIX_SERVICE_BIN` (default: meson `helix-service`).
3. **Parity before delete.** Nested or unit tests that cover a domain must pass against the Rust binary before that C++ domain is removed.
4. **Same change deletes C++.** When a domain cuts over, remove its C++ sources and meson bits in that change.

## Dev selection

```sh
# C++ (default today)
meson compile -C build helix-service

# Rust scaffold
cargo build -p helix-desktop
HELIX_SERVICE_BIN=$PWD/target/debug/helix-desktop tools/nested/…
```

## Order

1. ~~Scaffold `helix-desktop` + risk policy in `helix-shell-core`~~
2. ~~Agent Surface (grants, Hello/Action/Confirm, ListTools, manager)~~ — Action still returns stub content (no IPC to shell yet)
3. ~~Configuration1 bootstrap catalog + writes~~ (theme/audio/accessibility/shell chrome; growing)
4. ~~Audio1 in-memory backend~~ (volume/mute/default; PipeWire later) · ~~Appearance1 GetSnapshot stub~~
5. ~~Network1 in-memory mock~~ (enable/scan/connect/disconnect/forget/vpn; no NM yet)
6. ~~Bluetooth1 in-memory mock~~ (power/scan/connect/pair/forget; no BlueZ yet)
7. ~~Displays1 GetSnapshot stub~~ (mock inventory when desktop registered; no niri query yet)
8. ~~Appearance1 builtin palette-previews + SetWallpaper path~~ (no Material expand / wallhaven / apply yet)
9. ~~Configuration1 RegisterCapabilities + Get/Set/ClearState + catalog growth~~ (ordered lists / CommitOverrides still stub)
10. ~~AgentSurfaceActivity1~~ (desktop-only kill switch, pending, grants view)
11. ~~Community/custom palette-previews~~ + ~~opt-in catalog HTTP sync~~ (`HELIX_COMMUNITY_PALETTE_CATALOG_URL`)
12. ~~UpdateApplicationList for dock/launcher pins~~ (other ordered-list types still stub)
13. Real backends (PW/NM/BlueZ/niri) · remaining ordered lists · theme apply
14. Assistant, then inference
