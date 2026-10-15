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

| Item | Size | Evidence / where |
|---|---|---|
| Merge the greeter-sync and docs branch | S | `land-greeter-sync-and-doc-reset` |
| Retire C++ dev tooling as one change: `lefthook.yml`, `justfile` (`format`, `lint`, `fix`, `_clang_tidy`), `ci.yml` clang-format/tidy jobs, PR template + `enforce-pr-template.py` + its tests, `tools/ci-local.sh`, CONTRIBUTING formatting section | S | All still name `cpp/`. **Check required status-check names in branch protection before removing CI jobs.** |
| Decide Rust formatting: one repo-wide `cargo fmt` commit plus a CI check, or no policy | S | Tree outside the touched files is not rustfmt-clean (hundreds of diffs in `core/`, Mobius) |
| Pin the toolchain (`rust-toolchain.toml` or document rustup-first `PATH`) | S | Fedora `rustc` + rustup `clippy-driver` on one `PATH` fails with "incompatible version of rustc" |
| One local gate: `tools/check-all.sh` stops saying "C++ tests"; layering + clippy + tests + contracts in one entry | S | `tools/check-all.sh` |
| Remove the `helix-settings -> helix-wallpaper` layering exception | S | `tools/layers-baseline.txt` |
| Fix the stale ASan section and C++ dependency list in `BUILDING.md` / `CONTRIBUTING.md` | S | They describe `build-asan/helix-assistant` and C++23 |
| Record the pending budgets (USS/RSS) for Software and System Monitor | S | `software-tauri.md`, `system-monitor-tauri.md` budget tables read _pending_ |
| Run the soak checklist on the real session now that C++ is gone | M | `rust-desktop-soak.md` |

## 2. Settings (missing pages and ghost settings)

Settings has pages for Appearance, Accessibility, Displays, Network, Bluetooth, Keyboard, Wallpaper, Dock,
Power & lock, Privacy & agents and About. The gaps below come from `settings-audit.md` and `settings-tauri.md`.

| Item | Size | Needs | Evidence |
|---|---|---|---|
| Sound page: live input level meter | S | A `watch_input_level` call in `helix-desktop-api` for the `InputLevel` signal; `Audio1.StartInputMeter` is already there | The page ships without the meter (device choice, volume and mute are done) |
| **Notifications page**: do-not-disturb schedule, per-app rules, toast count and duration | M | A consumer in `helix_shell_core::notifications`; the Tauri notification server reads none of the old rows | `settings-audit.md` |
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
| **Act loop end to end**: Mobius → Assistant → confirmer → result evidence shown back in the conversation | M | Trace what exists today | `mobius-product.md` priority 5, "Show task progress and the resulting state" |
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
