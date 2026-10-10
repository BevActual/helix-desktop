# Helix Software (Tauri)

Standalone package store as a Tauri toplevel. Pure package models live in `helix-shell-core::software`; the host runs Flatpak / PackageKit / fwupd and draws text-only UI from `core/shared-ui`.

## Threat model

- Install / remove / update are mutations: the UI confirms before the host runs a backend command; nested e2e never performs a real install (fakes only).
- Remotes and package ids shown in the UI are sanitized display strings (`textContent`); no HTML from remotes or descriptions.
- Secrets do not appear (no passwords in this app). Logs never include package payload bodies.
- `--app=<id>` deep-link only accepts a short allowlisted id shape (alphanumeric, `.`, `-`, `_`); unknown ids open Explore empty.
- The app does not hold Agent Surface roles and does not talk to the desktop service for v1 catalog work.

## Backends

| Backend | Pages | Host commands (thin) |
|---------|-------|----------------------|
| Flatpak | Explore, Library, Updates, Sources | `flatpak remote-ls`, `list`, `update --appstream`, install/uninstall (queued) |
| Fedora PackageKit | Explore (rpm), Library, Updates | PackageKit D-Bus via existing patterns / `pkcon` where needed |
| Firmware | Updates (fwupd section) | `fwupdmgr get-updates` / apply (confirm) |

v1 ships Flatpak Explore / Library / Updates first; PackageKit + firmware + Sources land in the same crate next.

## Page map

- **Explore** — featured / search / categories; install confirm.
- **Library** — installed apps; open / uninstall.
- **Updates** — pending updates; update-one / update-all confirm.
- **Sources** — Flatpak remotes add/remove (Flathub helper).
- **Detail** — one package (`--app=` or row click).

## Budgets

Own process, not shell USS. Record app USS/RSS after the first nested run below.

| Measurement | Value | Notes |
|-------------|-------|-------|
| Nested first paint (fake catalog) | _pending_ | `tools/nested/software-test.sh` |
| USS after first nested run | _pending_ | |

## Nested tests

`tools/nested/software-test.sh` against a fake Flatpak catalog: page controls exist, search filters, install confirm arms without running `flatpak`.
