# Roadmap and backlog

The single place that says what is open and in what order. It is built from the design docs and the code as of
2026-10-15; each row names where the gap was found, so a row can be re-checked rather than trusted. When an item
lands, delete the row and update the doc that owns the behaviour. Structure and ownership: `product-structure.md`.

**Size:** S is under a day, M is a few days, L is a week or more. **Needs** is what must exist first.

## Where the project stands

The C++ → Rust cutover is finished (`cpp-to-rust.md`). Helix is a Tauri/Rust Fedora desktop on a niri fork. The bet
that sets it apart is the agentic OS (`agentic-os.md`): Agent Surface, Mobius, the confirmer and grants. Everything
below either makes the desktop complete enough to live in, or makes that bet trustworthy.

## 1. Foundations (do first, all small)

Done on 2026-10-15: the C++ cutover, the Cargo-only build (`tools/install.sh`, `tools/test-integration.sh`, no Meson), removal of the
Nix, Guix and AUR packaging that built the deleted C++ binary, and a Rust layering check. What is left:

| Item | Size | Evidence / where |
|---|---|---|
| Merge the `land-greeter-sync-and-doc-reset` branch | S | Greeter appearance sync, Sound page, Act task card, build and docs reset |
| Pin the toolchain (`rust-toolchain.toml`) | S | Fedora `rustc` with a rustup `clippy-driver` on one `PATH` fails with "incompatible version of rustc" |
| RPM follow-ups (`PACKAGING.md` "Not done"): a COPR project with automatic builds, a CI job for `tools/packaging/rpm-test.sh`, signing, aarch64, SELinux for the greeter | M | `packaging/helix.spec` exists | `PACKAGING.md` |
| Move CI to a Fedora container and install the real dependency list from `BUILDING.md` | M | CI builds on Arch with a list trimmed from the C++ one, so it is not the platform we ship on |
| Audit the remaining `docs/user` pages (configuration, services, launcher, control-center, theming, ipc) against what the Tauri shell does, rewrite or retire each | M | Most still describe the previous TOML-configured shell; the overview says so |
| Run the soak checklist on the real session now that C++ is gone | M | `rust-desktop-soak.md` |
| Run `mobius-test.sh` again (it now starts the Rust inference service) and fix what has rotted | S | `tools/nested/mobius-nested.sh` |
| Decide on `third_party/wuffs` and `third_party/material_color_utilities`: build the image-decode plan on Wuffs or remove it; keep the C++ reference only while the HCT goldens need regenerating | S | Neither is built today |
| Find out why `inference_service_integration` is SIGKILLed after about 85 s (it also happened here and took the calling shell with it), then make it a CI gate | S | `tools/test-integration.sh inference_service_integration`; the note in `ci.yml` |
| Compositor in the workspace: decide the release profile for the rest of the dependency tree. `niri`, `niri-config`, `niri-ipc` and `smithay` are set to opt-level 3 with overflow checks, but crates like `calloop` and the Wayland libraries follow the workspace's opt-level "s" | S | Measure frame times in a nested run before and after; `Cargo.toml` `[profile.release.package.*]` |
| Local disk: `target/` is 100 GB and `build-native-clean/` 2 GB of stale artifacts | S | `cargo clean`, or `cargo sweep` to keep the current toolchain's output |

## 2. Settings (missing pages and ghost settings)

Settings has pages for Appearance, Accessibility, Displays, Network, Bluetooth, Keyboard, Wallpaper, Dock,
Notifications, Power & lock, Privacy & agents and About. The gaps below come from `settings-audit.md` and `settings-tauri.md`.

| Item | Size | Needs | Evidence |
|---|---|---|---|
| **Notifications page, next**: per-app rules (quiet hours, the on/off switch, banner time and count are done and kept in one file with Quick Settings) | M | A consumer in `helix_shell_model::notifications` | `settings-audit.md` |
| **Night light**: temperature and schedule | L | A gamma-control client (`wlr-gamma-control` is in `protocols/`); the service only guards the setting today | `settings-audit.md`; `configuration.rs` has guards, no gamma client |
| **Interface scale in the shell's own chrome** (bar, dock, panels follow the scale) | M | The bar and dock set layer-surface sizes and exclusive zones, so they need to follow the scale rather than be zoomed | Applications follow `accessibility.ui_scale` already; the shell does not |
| Candidates, not yet confirmed as needed: date/time and region, default applications, pointer and touchpad | M each | Product decision | Common desktop expectations; no doc asks for them yet |

Rule for any new row (from `settings-audit.md`): nothing ships in Settings unless something running in the session
acts on it, and a nested test asserts the exact requests sent.

## 3. Apps

| Item | Size | Needs | Evidence |
|---|---|---|---|
| Software: a real install, remove and update tested with authentication on a throwaway machine; adding and removing sources | M | A Fedora VM or container with PackageKit | `software-tauri.md` "Fedora applications" |
| Software: add and remove sources | M | Confirmation and auth for system remotes | `software-tauri.md` |
| System Monitor: kill and renice, behind a confirmation | M | Own-process-only rule, confirm dialog | `system-monitor-tauri.md` "v1 omissions" |
| System Monitor: cut the bar and Control Center `sysmon` widgets over | M | Monitor model shared with the shell | `system-monitor-tauri.md` |
| Greeter: power actions (restart, shut down) | M | logind call from the greeter user, polkit | `greeter-tauri.md` "Not yet" |
| Greeter: remember last account and session | S | State file in `/var/lib/helix-greeter` | `greeter-tauri.md` |
| Greeter: packaging for the apply helper and a polkit rule scoped to the installed path | M | Install layout | `greeter-tauri.md`; the helper is safe to expose only at a fixed, root-owned path |

## 4. Shell

| Item | Size | Needs | Evidence |
|---|---|---|---|
| Finish the React slices and delete the hand-written fallbacks | M–L | None | `frontend-stack.md`: `shell.js` still owns mode, regions and glass |
| Mic and camera in use indicator | M | Service exposes capture streams | `tauri-rust-shell.md` "Not yet" |
| Today panel with calendar behind the date, and timers | M | A calendar source | `tauri-rust-shell.md` |
| Extension story to replace the removed Luau plugins | L | Design doc first (sandboxed, out-of-process over D-Bus fits the contracts) | README "Scope" |

## 5. Agentic OS (the differentiating work)

| Item | Size | Needs | Evidence |
|---|---|---|---|
| Act loop, next step: show the resulting state (what changed, not only the shell's sentence) and offer undo where an action can be undone | M | The live restore commands reply with the pair. `GetState` carries `before`, `after`, and `undoable`, and `Undo` writes the previous value back. The chat note and the shell's Act door show Undo while the shell still says that task can be undone. Confirm-tier undo is not built. `act-undo.md` | `mobius-product.md` priority 5 |
| Tier 3 (synthetic input): decide the three open questions in `synthetic-input-threat-model.md`, then build in its order (semantic AT-SPI on agent-desk windows first) | M | The person's decisions; the keyboard path in `trusted-surface.md` proven first | `synthetic-input-threat-model.md` |
| Project knowledge: explicit notes and selected files reused across chats, with visible sources | L | The store and its measurement are built (`knowledge.md`); next is the Mobius wiring, a Rust `Embed` over `helix-embed`, and cited sources | `mobius-product.md` priority 4 |
| Read-aloud replies with playback and cancel | M | TTS engine choice | `mobius-product.md` |
| Act: the eval's remaining findings (`agent-and-inference.md`): the 7B still answers a status tool for "turn off Wi-Fi" and similar; refresh the stale decline cases; then measure Tools-mode planning and retrieval, which have no evaluation yet | M | The harness exists (`tools/run-assistant-eval.sh`) | `agent-and-inference.md` |
| Replace the llama, whisper and image C/C++ shims with Rust, or fix the boundary as permanent | L | Decision | `cpp-to-rust.md` "Next" |

## Suggested order

1. Foundations, all of section 1 (a few days, unlocks everything).
2. Act loop end to end: the product bet, and it decides what the agent-facing UI work looks like.
3. Software: a real install tested on a throwaway machine, then adding and removing sources.
4. Notifications page together with its consumer, then night light, then UI scale. The Sound page, including the microphone test, is done. Quiet hours hide ordinary banners between two hours and are checked when the hour changes (`notifications-prefs-test.sh`). Displays sets resolution, scale, orientation, arrangement, variable refresh, and power, and keeps them in `helix-setup.kdl` (`settings-displays-test.sh`). Night light still needs a gamma client before it can be a row. Per-app notification rules are still open.
5. Greeter power actions and packaging, then shell slices and the extension design.
