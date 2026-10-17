# Helix Software (Tauri)

Frontend: React + TypeScript + Vite (`apps/helix-software/web`, workspace `helix-software-web`, built into `dist/react/helix-software.js`); the hand-written page in `dist/software.js` is the fallback for a build without node. Rust still decides what is listed and what a mutation means; the page draws `PageView` and sends back an id and one of install/remove/update, then Allow or Cancel.

The React page adds: package icons, Installed / Update available chips, the update count on the sidebar, a named confirmation ("Install Demo App?"), a working state and an error banner, per-page empty states, library filtering, `/` to focus search, and a highlighted row for `--app=`.

Standalone package store as a Tauri toplevel. Pure package models live in `helix-software-model::software`; the host runs Flatpak / PackageKit / fwupd and draws text-only UI from `core/shared-ui`.

## Threat model

- Install / remove / update are mutations: the UI confirms before the host runs a backend command; nested e2e never performs a real install (fakes only).
- Remotes and package ids shown in the UI are sanitized display strings (`textContent`); no HTML from remotes or descriptions.
- Secrets do not appear (no passwords in this app). Logs never include package payload bodies.
- `--app=<id>` deep-link only accepts a short allowlisted id shape (alphanumeric, `.`, `-`, `_`); unknown ids open Explore empty.
- The app does not hold Agent Surface roles and does not talk to the desktop service for v1 catalog work.
- PackageKit changes use only package names the store already lists (from the distribution's catalog), resolved to ids by PackageKit itself; the page sends an id and an action, never a package spec. Removal never takes dependents with it.

## What it does today (Flatpak)

| Area | Behaviour |
|---|---|
| Catalog | `flatpak remote-ls --cached` per installation (user, system) merged with `flatpak list` and `remote-ls --updates`; one row per application id, a user remote winning (installs there need no authentication). Read in the background; the window opens first and shows a skeleton. The installed apps still show when a remote has no cached summary. |
| Discover | The landing page, built from the appstream index read once per catalog (categories, newest release, publisher verified): a **Featured** hero tinted from the app's icon, a *Recently updated* shelf, and a shelf per category (Productivity, Graphics & Design, Audio & Video, Developer Tools, Games, Internet, Education, Science, Utilities) with See All. The hero rotates daily among verified, recent apps. Apps without an icon are kept off the shelves; with no appstream data it falls back to the plain list. |
| Categories | The sidebar lists them; a category page is the ranked list scoped to it, paged as it scrolls, with search inside it. |
| Explore (search) | Ranked search (exact, prefix, word prefix, contains, id, summary) over thousands of apps, served in pages of 60 as the list scrolls. Icons are the appstream icons of the remote, loaded lazily. |
| Detail | Description, developer, licence, categories and release from the remote's appstream data (`zcat` + a component scan, on demand), plus version, size and source, and **screenshots** fetched on demand from the remote's own media host (`https://dl.flathub.org/` only, images only, 3 MB cap, cached under `~/.cache/helix/software/screens`). Nothing is fetched until a detail page is opened. |
| Jobs | Install, remove, update and Update All run as jobs on their own thread with progress parsed from Flatpak's output, shown as a **circular progress control** (the square inside cancels) and a floating bar, a result that explains a failure, and successes that dismiss themselves. Only one mutation per package at a time; the page never builds a command. |
| Library | Installed apps with **Open** (`flatpak run`, detached); Remove from the detail page. |
| Updates | The count on the sidebar, **Check for Updates** (fetches appstream first, falls back to the cached catalog offline), **Update All** behind one confirmation. Firmware updates (fwupd) are listed and applied by the system. |
| Sources | The configured remotes, their URLs and whether they are user or system. |

Tests: `cargo test -p helix-software` (parsers), `tools/nested/ui-suite.sh software` (fake catalog, windowed list, search, detail, install
with progress, Update All) and `software-real` (this machine's real catalog and library, read-only; skipped without Flatpak apps).

Not done: adding or removing sources, screenshots for Fedora applications, ratings, and Fedora repositories on the Sources page.

## Fedora applications (PackageKit)

`apps/helix-software/src-tauri/src/rpm.rs`. Flatpak and Fedora applications share Explore, Discover, Library and Updates (`STORE_KINDS` in the model); a row says which it is
by its source. Where PackageKit is not on the system bus (another distribution) the store is Flatpak only, and with no Flatpak it is Fedora only.

| Area | Behaviour |
|---|---|
| Catalog | The distribution's AppStream data on disk (`/usr/share/swcatalog`, `/var/cache/swcatalog`, `/usr/share/app-info`; `.xml` and `.xml.gz`): desktop applications that name a package, one row per package, with name, summary, categories, newest release and the cached icon. About a thousand applications on Fedora 44 (the rest of the 2,300 components are not applications or have no valid package name). The package name is the row's id. |
| Installed and updates | `GetPackages(installed)` and `GetUpdates` on a PackageKit transaction, merged by name and the machine's architecture. An update is offered only for an application that is installed. Every other package update is **one row, "System software"**, so the Updates page is not a list of 200 libraries; updating it runs those packages and the dialog says some (the kernel) take effect after a restart. |
| Install, remove, update | `InstallPackages`, `RemovePackages`, `UpdatePackages` on a PackageKit transaction, with progress from `ItemProgress` and `Percentage`, and **Cancel** calling the transaction's `Cancel`. Install resolves the name to the first *available* package of this machine's architecture (or noarch), never another one. Remove passes `allow_deps = false` and `autoremove = false`: if another package needs it, PackageKit refuses and the row says so in its words, and nothing else is removed. Update All runs the Flatpak update and then one PackageKit update of everything. |
| Authentication | PackageKit asks polkit, and the shell's polkit agent shows the dialog, as for a system Flatpak. The app never handles a password. |
| Open | `gtk-launch <desktop id>` for the entry the catalog names. |
| Detail | The description, licence and categories come from the same catalog (read on demand). Fedora applications show no screenshots (the catalog's hosts are not on the allowed list). |
| Check for Updates | `RefreshCache` on a PackageKit transaction, after the Flatpak appstream refresh. |

Tests: `rpm.rs` unit tests (catalog parsing, package ids, the merge, icon paths); `tools/nested/software-rpm-test.sh` runs the real client against `tools/nested/fake-packagekit.py` on a
private bus with a fixture catalog (library, search, install with progress and architecture, a refused removal, cancel, Update All); and
`cargo test -p helix-software -- --ignored --nocapture real_` reads this machine's real catalog and PackageKit (read only).
Not tested: a real install, removal or update (it changes the system and needs authentication).

## Backends

| Backend | Pages | Host commands (thin) |
|---------|-------|----------------------|
| Flatpak | Explore, Library, Updates, Sources | `flatpak remote-ls`, `list`, `update --appstream`, install/uninstall (queued) |
| Fedora PackageKit | Explore, Library, Updates | PackageKit D-Bus (`rpm.rs`) |
| Firmware | Updates (fwupd section) | `fwupdmgr get-updates` / apply (confirm) |

Flatpak, PackageKit and firmware updates are in; adding and removing sources is next.

## Page map

- **Explore** — featured / search / categories; install confirm.
- **Library** — installed apps; open / uninstall.
- **Updates** — pending updates; update-one / update-all confirm.
- **Sources** — Flatpak remotes add/remove (Flathub helper).
- **Detail** — one package (`--app=` or row click).

## Budgets

Own process, not shell USS. Memory is PSS of the app and its WebKit processes (a headless nested run, so a floor for what a real session holds).

| Measurement | Value | Notes |
|-------------|-------|-------|
| Nested first paint (fake catalog) | not measured | The nested compositor's start-up (about 14 s) dominates; it says nothing about the app |
| Memory after first nested run (PSS, app and its WebKit processes) | about 315 MB | `tools/nested/measure-app.sh` with `MEASURE_READY=page-discover MEASURE_BIN=helix-software`; unchanged 15 s later (314.7 MB). Measured 2026-10-16 on a `fast` build |

## Nested tests

`tools/nested/software-test.sh` against a fake Flatpak catalog: page controls exist, search filters, install confirm arms without running `flatpak`.
