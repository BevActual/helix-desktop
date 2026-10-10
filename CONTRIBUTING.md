Contributing
===

This file collects contributor-facing details for Helix Shell: design goals, stack notes, code style, source layout,
runtime asset behavior, and debugging helpers. Product contracts with in-tree `compositor/` and `greeter/`
are in their `HELIX.md` files and in `.cursor/rules/helix-hig.mdc`.

Product shape (greeter / compositor / shell / apps / services): [docs/design/product-structure.md](docs/design/product-structure.md).
Design index: [docs/design/README.md](docs/design/README.md).

For dependencies and normal build commands, start with [BUILDING.md](BUILDING.md).

## Design Principles

- Session shell and apps: Rust + Tauri 2 (GTK3 layer-shell / WebKit), shared UI in `core/shared-ui/`.
- Desktop authority: `org.helix.Desktop` (C++ `helix-service` today; migrating to Rust — see `docs/design/cpp-to-rust.md`).
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

### C++ platform (desktop service / assistant / inference — shrinking)

| Layer | Library |
|-------|---------|
| IPC and service runtime | `sdbus-c++`, `glib-2.0`, `gobject-2.0`, `gio-2.0` |
| Wayland helpers (service) | `libwayland-client`, `wayland-protocols` |
| Audio | `libpipewire-0.3`, `wireplumber-0.5`, `libsndfile` |
| Authentication | `PAM`, `polkit-agent-1`, `polkit-gobject-1` |
| Credentials and encryption | `libsecret-1`, `libsodium` |
| HTTP | `libcurl` |
| Config | `tomlplusplus` |
| JSON | `nlohmann/json` |
| Theme generation | Material Color Utilities (vendored) |
| Images (service helpers) | `libwebp`, `libjxl`, `librsvg` |
| Memory allocation | `jemalloc` (optional) |

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
src/
  main.cpp          Entry point
  app/              Application bootstrap, main loop, poll sources
  auth/             PAM and fingerprint authentication
  calendar/         CalDAV, Google Calendar, iCalendar parsing, polling
  capture/          Screenshots, screencopy capture, region overlay
  compositors/      Compositor detection, runtime adapters, workspace/output/keyboard backends
  config/           Configuration schema, validation, hot reload, state store, overrides
    schema/         Typed config schema engine
  core/             Logging, timers, process helpers, resource paths, shared utilities
  dbus/             Session/system bus wrappers and service integrations
    accounts/       AccountsService user metadata
    bluetooth/      BlueZ service and pairing agent
    idle/           Screensaver D-Bus service
    logind/         logind integration
    modem/          ModemManager cellular integration
    mpris/          Media player integration and artwork cache
    network/        NetworkManager, wpa_supplicant, and secret agent integration
    notification/   Desktop notification D-Bus service
    polkit/         Polkit authentication agent
    power/          power-profiles-daemon integration
    tray/           StatusNotifierItem watcher/host
    upower/         Battery and power device integration
  debug/            Debug D-Bus service
  hooks/            User hook state and hook manager
  i18n/             Translation catalog and language tag handling
  idle/             Idle manager, inhibitor, grace overlay
  ipc/              IPC client/service and CLI command parsing
  launcher/         Launcher providers (apps, emoji, calculator, sessions, windows)
  net/              HTTP client, URI parsing, URL opening
  notification/     Notification model, manager, filtering, history
  pipewire/         PipeWire audio service, sound playback, spectrum analyzer
  render/
    animation/      Animation manager, easing, motion settings
    backend/        GLES render backend, framebuffers, texture manager
    core/           Render data types, image decoders/encoders, texture caches
    programs/       GLES shader programs
    scene/          Scene graph nodes and pointer input dispatch
    text/           Cairo/Pango text and glyph rendering
  shell/
    backdrop/       Backdrop layer surfaces
    bar/            Bar surface, instance, widget base/factory
      widgets/      Bar widget implementations
    clipboard/      Clipboard history panel and paste helpers
    control_center/ Control center panel, tabs, shortcut registry
    desktop/        Desktop widget host, factory, layout, editor support
      widgets/      Desktop widget implementations
    dock/           Dock surface, model, pinned apps, context menu
    greeter/        Greeter appearance sync
    launcher/       Launcher panel
    lockscreen/     Session lock surfaces and lockscreen widgets
    notification/   Notification toasts
    osd/            On-screen display overlays
    overview/       Overview capture helpers
    panel/          Panel base, manager, attached-panel context
    polkit/         Polkit prompt panel
    screen_corners/ Screen corner overlays
    session/        Session panel and action runners
    settings/       Settings editors, setting controls, registries, sheets
    setup_wizard/   First-run setup wizard
    surface/        Shared shell surface geometry and shadow helpers
    switcher/       Window switcher UI
    tooltip/        Tooltip manager
    tray/           Tray drawer and D-Bus menu UI
    wallpaper/      Wallpaper surfaces, paths, picker panel
    widgets_editor/ Background widget editor
  system/           Desktop entries, brightness, weather, location, system monitor, hardware services
  theme/            Palette generation, templates, theme service, template application
  time/             Time service and polling
  ui/
    controls/       Reusable controls (Button, Input, Label, Select, Slider, Box, ...)
    dialogs/        File, color, and glyph picker dialogs
    visuals/        Shared visualizer controls
  util/             Generic helpers
  wayland/          Wayland connection, seats, surfaces, clipboard, toplevels, text input
    hyprland/       Hyprland-specific Wayland protocol helpers
assets/
  fonts/            Bundled Tabler and UI fonts
  sounds/           Notification and UI sounds
  templates/        Built-in theme templates
  translations/     Exported translation catalogs
protocols/          D-Bus + IPC contracts (source of truth); also vendored Wayland XML
tests/              Unit tests and config validation fixtures
tools/              Developer and translation helper scripts
nix/                Nix package, module, and dev shell definitions
third_party/
  wuffs/          Raster image decoding (vendored)
  fzy/            Fuzzy matching (vendored)
  material_color_utilities/ Material Design color generation (vendored)
```


## Debugging

All debug commands use the `org.helix.Debug` D-Bus service, available at runtime.

```sh
# Enable verbose debug logs
gdbus call --session --dest org.helix.Debug --object-path /dev/helix/Debug --method org.helix.Debug.SetVerboseLogs true

# Disable verbose debug logs
gdbus call --session --dest org.helix.Debug --object-path /dev/helix/Debug --method org.helix.Debug.SetVerboseLogs false

# Check current verbose log state
gdbus call --session --dest org.helix.Debug --object-path /dev/helix/Debug --method org.helix.Debug.GetVerboseLogs

# Emit an internal notification (app_name, summary, body, timeout_ms, urgency 0-2)
gdbus call --session --dest org.helix.Debug --object-path /dev/helix/Debug --method org.helix.Debug.EmitInternalNotification "Helix" "Test" "Hello from debug" 5000 1
```

### Crash output

When reporting a crash, include the complete terminal output from the process if it is available. Do not paste only
`Segmentation fault`; that line does not contain a useful stack trace or the error context.

### ASan crash reports

AddressSanitizer (ASan) can find memory errors that a normal build reports only as a crash. It requires a temporary
source build; it does not replace the Helix package you already use.

Install the source-build dependencies for your distribution using the commands in
[BUILDING.md](BUILDING.md#dependencies), then clone the repository:

```sh
git clone https://github.com/BevActual/helix-shell.git
cd helix-shell
```

Stop the Helix instance started by your compositor, then configure and build ASan:

```sh
just configure asan && just build asan
```

Start the ASan binary in the foreground and save its output:

```sh
ASAN_OPTIONS=log_path=/tmp/helix-asan ./build-asan/helix-service 2>&1 | tee helix-asan-terminal.log
```

Reproduce the crash once. If Helix does not exit, press `Ctrl+C`. Attach both `helix-asan-terminal.log` and every
`/tmp/helix-asan.*` file to the GitHub issue. The files in `/tmp` preserve the ASan report if the crash takes down
the terminal. If the build or startup fails, attach that complete output instead.
