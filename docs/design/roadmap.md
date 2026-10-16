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
| Retire the C++ formatting checks as one change: the `format` job in `ci.yml`, the PR template checkbox and `enforce-pr-template.py` + its tests, `tools/ci-local.sh`'s format job | S | They still ask for `just format` with clang-format over a `cpp/` tree. **Check required status-check names in branch protection before removing CI jobs.** |
| Decide Rust formatting: one repo-wide `cargo fmt` commit plus a CI check, or no policy | S | The tree outside touched files is not rustfmt-clean (hundreds of diffs in `core/`, Mobius) |
| Pin the toolchain (`rust-toolchain.toml`) | S | Fedora `rustc` with a rustup `clippy-driver` on one `PATH` fails with "incompatible version of rustc" |
| Write the RPM spec over `tools/install.sh --require-session`, with a systemd user preset and a decision on how the compositor is packaged | M | `PACKAGING.md`; the installer now covers the whole session |
| Move CI to a Fedora container and install the real dependency list from `BUILDING.md` | M | CI builds on Arch with a list trimmed from the C++ one, so it is not the platform we ship on |
| Audit the remaining `docs/user` pages (configuration, services, launcher, control-center, theming, ipc) against what the Tauri shell does, rewrite or retire each | M | Most still describe the previous TOML-configured shell; the overview says so |
| Remove the `helix-settings -> helix-wallpaper` layering exception | S | `tools/layers-baseline.txt` |
| Record the pending budgets (USS/RSS) for Software and System Monitor | S | `software-tauri.md`, `system-monitor-tauri.md` budget tables read _pending_ |
| Run the soak checklist on the real session now that C++ is gone | M | `rust-desktop-soak.md` |
| Run `mobius-test.sh` again (it now starts the Rust inference service) and fix what has rotted | S | `tools/nested/mobius-nested.sh` |
| Decide on `third_party/wuffs` and `third_party/material_color_utilities`: build the image-decode plan on Wuffs or remove it; keep the C++ reference only while the HCT goldens need regenerating | S | Neither is built today |
| Find out why `inference_service_integration` is SIGKILLed after about 85 s (it also happened here and took the calling shell with it), then make it a CI gate | S | `tools/test-integration.sh inference_service_integration`; the note in `ci.yml` |
| Local disk: `target/` is 100 GB and `build-native-clean/` 2 GB of stale artifacts | S | `cargo clean`, or `cargo sweep` to keep the current toolchain's output |

## 2. Settings (missing pages and ghost settings)

Settings has pages for Appearance, Accessibility, Displays, Network, Bluetooth, Keyboard, Wallpaper, Dock,
Power & lock, Privacy & agents and About. The gaps below come from `settings-audit.md` and `settings-tauri.md`.

| Item | Size | Needs | Evidence |
|---|---|---|---|
| Sound page: live input level meter | S | A `watch_input_level` call in `helix-desktop-api` for the `InputLevel` signal; `Audio1.StartInputMeter` is already there | The page ships without the meter (device choice, volume and mute are done) |
| **Notifications page**: do-not-disturb schedule, per-app rules, toast count and duration | M | A consumer in `helix_shell_model::notifications`; the Tauri notification server reads none of the old rows | `settings-audit.md` |
| **Night light**: temperature and schedule | L | A gamma-control client (`wlr-gamma-control` is in `protocols/`); the service only guards the setting today | `settings-audit.md`; `configuration.rs` has guards, no gamma client |
| **Text size / UI scale** | M | A scale applied by the shell and Settings | `accessibility.ui_scale` has no live consumer except the greeter (it now reads it through the sync helper) |
| Candidates, not yet confirmed as needed: date/time and region, default applications, pointer and touchpad | M each | Product decision | Common desktop expectations; no doc asks for them yet |

Rule for any new row (from `settings-audit.md`): nothing ships in Settings unless something running in the session
acts on it, and a nested test asserts the exact requests sent.

## 3. Apps

| Item | Size | Needs | Evidence |
|---|---|---|---|
| **Software: Fedora RPM backend (PackageKit)** | L | Backend trait, authentication flow | Fedora is the sole target and the store lists Flatpak only (`software-tauri.md`: "Not done") |
| Software: add and remove sources | M | Confirmation and auth for system remotes | `software-tauri.md` |
| Software and Monitor follow the system light theme | S | Appearance mode exposed to apps | Apps render dark today |
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
| Act loop, next step: keep the finished task in the conversation, show the resulting state, and offer undo where the action can be undone | M | The task card (landed) | `mobius-product.md` priority 5. Today the card is above the composer and is not saved with the chat |
| Threat model for tier 3 (synthetic input) before any implementation | S | None | `agentic-os.md` marks it phase 2; `trusted-surface.md` |
| Project knowledge: explicit notes and selected files reused across chats, with visible sources | L | Retrieval measured first | `mobius-product.md` priority 4 |
| Read-aloud replies with playback and cancel | M | TTS engine choice | `mobius-product.md` |
| Measured evals for model-dependent behaviour (Tools-mode planning, retrieval) | M | `tests/eval` | `mobius-product.md` "How to judge a delivery" |
| Replace the llama, whisper and image C/C++ shims with Rust, or fix the boundary as permanent | L | Decision | `cpp-to-rust.md` "Next" |

## Suggested order

1. Foundations, all of section 1 (a few days, unlocks everything).
2. Sound page: biggest visible hole, no dependencies, and it exercises the page pattern the others reuse.
3. Act loop end to end: the product bet, and it decides what the agent-facing UI work looks like.
4. Software RPM backend: the largest functional gap for a Fedora-only product.
5. Notifications page together with its consumer, then night light, then UI scale.
6. Greeter power actions and packaging, then shell slices and the extension design.
