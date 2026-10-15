Contributing
===

This file collects contributor-facing details for Helix Shell: design goals, stack notes, code style, source layout,
runtime asset behavior, and debugging helpers. Product contracts with in-tree `compositor/` and `greeter/`
are in their `HELIX.md` files and in `.cursor/rules/helix-hig.mdc`.

Product shape (greeter / compositor / shell / apps / services): [docs/design/product-structure.md](docs/design/product-structure.md).
Design index: [docs/design/README.md](docs/design/README.md).

For dependencies and normal build commands, start with [BUILDING.md](BUILDING.md).

## Git remotes and GitHub CLI

**[BevActual/helix-desktop](https://github.com/BevActual/helix-desktop)** is the product repository. It is **not** a GitHub fork of Noctalia; PRs and releases happen here only. **Default branch is `main`** (use `gh repo set-default BevActual/helix-desktop` and `tools/git-helix-origin.sh` if `git pull` still tracks an old branch).

Clone and work against `origin`:

```sh
git clone https://github.com/BevActual/helix-desktop.git
cd helix-desktop
gh repo set-default BevActual/helix-desktop
```

Older clones may still have `upstream` → `noctalia-dev/noctalia` and `main` tracking that remote. That breaks **`gh pr create`** (wrong base repo). Fix once:

```sh
tools/git-helix-origin.sh
```

Or manually: `git config branch.main.remote origin`, `git fetch origin`, `git checkout main && git pull`, and remove the legacy `upstream` remote if you do not need Noctalia history.

For `gh` without changing remotes, use `tools/gh-pr.sh` or:

```sh
gh pr create --repo BevActual/helix-desktop --base main --head your-branch
```

## Design Principles

- Session shell and apps: Rust + Tauri 2 (GTK3 layer-shell / WebKit), shared UI in `core/shared-ui/`.
- Desktop authority: `org.helix.Desktop` (Rust `helix-desktop`; see `docs/design/cpp-to-rust.md`).
- Applications and Controls: GTK4/libadwaita grammar for look; Fedora + helix-niri is the shell target.
- Contracts in `protocols/` (D-Bus + IPC) are the boundary; do not invent parallel APIs.

## Stack

Direct project dependencies are listed below; transitive dependencies are owned by their providing system packages.

### Rust (shell, apps, greeter, migrating services)

| Layer | Library |
|-------|---------|
| App framework | Tauri 2, GTK3, WebKitGTK 4.1, `gtk-layer-shell` |
| D-Bus client/server | `zbus`, `tokio` |
| Serialization | `serde`, `serde_json` |
| TypeScript bindings | `ts-rs` (feature `ts`, `tools/gen-bindings.sh`) |

### Frontend (apps under `apps/` and the session shell)

| Layer | Library |
|-------|---------|
| UI | React 19, TypeScript, Vite (npm workspace at the repo root, Node 22+) |
| Shared components | `@helix/ui` in `core/shared-ui/react/` over the shared CSS tokens |
| Lint | ESLint with `react/no-danger`; text from models, apps and the system is only ever a React child |

The approval dialog and the greeter are hand-written HTML/CSS/JS with no build step; the shell is moving to React in slices. Rules and status: `docs/design/frontend-stack.md`.

### Meson

Meson installs units, policies, and assets. It does not compile platform C++. Session services, config, theme, and inference workers are Cargo.

| Layer | Library |
|-------|---------|
| Config export and validation | Rust (`helix-shell-core`) |
| Theme generation | Rust (`helix-theme`, vendored `material-colors`) |
| Desktop bus | Rust (`helix-desktop`) |

## Runtime Assets

`meson install` installs the binary and shipped assets separately using the normal prefix layout:

```text
/usr/local/bin/helix
/usr/local/share/helix/assets/...
```

With a different Meson `prefix`/`datadir`, the same structure is preserved under that prefix.

Helix Shell needs the `assets/` tree at runtime. Copying only the bare `helix-shell` binary is not enough.

Portable bundle layouts are also supported:

```text
bundle/
  helix
  assets/
```

```text
bundle/
  bin/helix
  share/helix/assets/
```

Runtime asset lookup order:

1. `HELIX_ASSETS_DIR`
2. `assets/` next to the executable
3. `assets/` one level above the executable
4. install-style `../share/helix/assets` relative to the executable
5. the compiled install path from Meson (`<prefix>/<datadir>/helix/assets`)
6. the source-tree `assets/` directory as a development fallback

An asset root is only accepted if it contains the expected shipped files such as `emoji.json`, `fonts/helix-tabler.ttf`,
`templates/builtin.toml`, and `translations/en.json`.

## Code Style

This project uses [clang-format](https://clang.llvm.org/docs/ClangFormat.html) for formatting. Run `just format`
before committing.

For editor integration, `just configure` creates a root `compile_commands.json` symlink to the selected Meson build
directory. Run `just configure`, `just configure release`, or `just configure asan` for the build mode you want clangd
to use.

The repo also includes `lefthook.yml`. Run `lefthook install` to install the pre-commit hook; it runs `just format`
before commits and refreshes the git index for tracked formatting changes.

Do not use em dashes (—) or double hyphens (--) as sentence punctuation, in code, comments, documentation, or commit
messages. Use a comma, colon, semicolon, or parentheses instead.

## Pull Request Template

Pull request descriptions are checked automatically when they are opened, edited, reopened, or marked ready for
review. Keep the `## Summary`, `## Motivation`, `## Type of Change`, `## Testing`, and `## Checklist` headings and the
Checklist wording from [`.github/PULL_REQUEST_TEMPLATE.md`](.github/PULL_REQUEST_TEMPLATE.md). The remaining sections
are context only: fill them in, leave them empty, or delete them. In Type of Change, keep only the lines that apply.

Draft pull requests may leave checkboxes incomplete. Before marking a pull request ready for review, select at least one
change type and check every item in the Checklist section. A pull request that is missing required template structure
is commented on and converted back to a draft; add the missing content and mark it ready for review to run the check
again. The check never closes a pull request.

### Naming Conventions

| | Convention | Example |
|---|---|---|
| Files | snake_case | `widget_factory.cpp` |
| Directories | snake_case | `shell/bar/widgets/` |
| Types / Classes | PascalCase | `WidgetFactory` |
| Functions / Methods | camelCase | `createWidget()` |
| Variables / Parameters | camelCase | `busName` |
| Private members | m_camelCase | `m_changeCallback` |
| Macros / Enum values | SCREAMING_SNAKE_CASE | `MAX_SIZE` |

D-Bus wire-protocol string literals, such as `player["bus_name"]`, stay snake_case because they are wire names, not
C++ identifiers.

## Translations

Helix translations ship as JSON under `assets/translations/`. Optional push/pull tools use `I18N_API_BASE` / `TRANSLATION_API_URL` when you run a translation service. The JSON
files in `assets/translations/` are exported from that workflow, with `assets/translations/en.json` acting as the
source catalog for new strings.

When a code, UI, settings, or documentation change needs a new user-facing string, add or update the English string in
`assets/translations/en.json` only. Do not machine translate strings, copy English into other locales, or include broad
updates to non-English translation files in a normal feature or bug-fix PR. The translation team handles those locale
updates through the translation app.

Only edit non-English translation files when the PR is explicitly about translation tooling, an import/export sync, or a
maintainer has asked for that specific locale change.

After adding or renaming translation keys, run:

```sh
python3 tools/i18n-check.py
```

## Project Layout

```text
Cargo.toml          Rust workspace (excludes compositor/)
core/               Pure models and typed clients
  helix-shell-core    Config catalog/export, agent risk policy, app models
  helix-desktop-api   Typed client for org.helix.Desktop
  helix-theme         Palettes, wallpaper schemes, template apply
  shared-ui/          CSS tokens, icons, @helix/ui React components, generated bindings
services/           One process each
  helix-desktop       org.helix.Desktop authority (audio, network, bluetooth, displays, appearance, agent surface)
  helix-assistant     Explicit Act presence (Agent Surface client)
  helix-inference     Inference fabric, helix-transcribe; native/ holds the llama and whisper shims
  helix-image         Image worker; native/ holds the C engine shim
  helix-system-upgrade, helix-wallpaper, helix-idle
shell/              Live session shell (Tauri + React islands)
apps/               Settings, Mobius, Software, System Monitor (Tauri + React)
greeter/            Login greeter (Tauri + React, greetd client)
compositor/         helix-niri fork (own Cargo workspace)
protocols/          D-Bus + IPC contracts (source of truth); also vendored Wayland XML
assets/             Icons, fonts, sounds, templates, translations, unit files
tests/              Python integration tests, fixtures, eval
tools/              Install, nested e2e, layering check, generators
dev/                Nested probes and test tooling
third_party/        Vendored crates and libraries
docs/design/        Architecture + HIG (see README.md); archive/ is retired
```

Structure and ownership rules: [docs/design/product-structure.md](docs/design/product-structure.md). Dependency direction is checked
by `python3 tools/check-layers.py .`.

## Debugging

Run the desktop service against mock backends with `HELIX_DESKTOP_MOCK_AUDIO=1`, `HELIX_DESKTOP_MOCK_NETWORK=1`,
`HELIX_DESKTOP_MOCK_BLUETOOTH=1` and `HELIX_DESKTOP_MOCK_DISPLAYS=1`. The nested harnesses under `tools/nested/` and the
hardware checklist in `docs/design/rust-desktop-soak.md` show how.

### Crash output

When reporting a crash, include the complete terminal output from the process if it is available. Do not paste only
`Segmentation fault`; that line does not contain a useful stack trace or the error context.

### ASan crash reports

AddressSanitizer (ASan) can find memory errors that a normal build reports only as a crash. It requires a temporary
source build; it does not replace the Helix package you already use.

Install the source-build dependencies for your distribution using the commands in
[BUILDING.md](BUILDING.md#dependencies), then clone the repository:

```sh
git clone https://github.com/BevActual/helix-desktop.git
cd helix-desktop
```

Stop the Helix instance started by your compositor, then configure and build ASan:

```sh
just configure asan && just build asan
```

Start the ASan binary in the foreground and save its output:

```sh
ASAN_OPTIONS=log_path=/tmp/helix-asan ./build-asan/helix-assistant 2>&1 | tee helix-asan-terminal.log
```

Reproduce the crash once. If Helix does not exit, press `Ctrl+C`. Attach both `helix-asan-terminal.log` and every
`/tmp/helix-asan.*` file to the GitHub issue. The files in `/tmp` preserve the ASan report if the crash takes down
the terminal. If the build or startup fails, attach that complete output instead.
