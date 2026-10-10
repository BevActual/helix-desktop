# Source layering

`src/` is checked for dependency direction by `tools/check-layers.py` (meson test `layering`). Rules and module
assignments live in `tools/layers.toml`; today's known violations are in `tools/layers-baseline.txt`.

## Layers

A module may include its own layer and the layers below it.

| Layer | Modules | Meaning |
|---|---|---|
| base | util, core, i18n, time | Standard-library level types and helpers. No knowledge of config, rendering or the shell. |
| platform | config, net, security, ipc, dbus, pipewire, system, theme, notification, hooks, auth, debug, idle, cli, desktop_api, wayland, compositors | Services and clients that do not draw anything. |
| display | render, ui | The shell's renderer and UI toolkit. |
| shell | shell, launcher, capture, app | Shell-only features and the application that wires them together. |

`desktop` (the desktop service), `inference` (the model runtime) and `apps` (Settings, Software, System Monitor) are **independent processes**. They may
include base, platform and each other, never display or shell, so they can run without the shell. They currently have
no violations. `desktop_api` is the client library for the service (protocol, client, display text); every process uses it.

## The ratchet

- A **new** violation fails `layering`. Fix the dependency: move the shared piece down a layer, or pass it in.
- A baseline entry that no longer exists also fails, so a fix must remove its line
  (`tools/check-layers.py . --update-baseline` rewrites the file). The list can only shrink.
- `tools/check-layers.py . --list` groups the remaining violations by module pair.

## What has moved so far

The baseline went from 106 to **10**. Moves, in the order they were made:

- `desktop_api/`: the client library for the desktop service (protocol, client, display text, route table, network proxy).
- `core/`: `poll_source.h` and `color.h` (27 modules reached up for them), `motion_durations.h`, `style_defaults.h`,
  `image_decoder` and `image_orientation`, `input/keybind_action.h`, `input/text_input_client.h`.
- `theme/color_roles`: `ColorRole`, `ColorSpec`, `Palette` and the role/token helpers (plain data).
- `config/`: widget gesture and action parsing, launcher provider names.
- `net/wallhaven_client`.
- `render/surface/`: `Surface`, `LayerSurface`, `PopupSurface`, `Subsurface`, `ToplevelSurface` (windowing glue that
  owns render targets; it was in `wayland/`). `render/scene/tooltip_content.h`, `ui/shadow.*`.
- `shell/idle/idle_grace_overlay.*` (UI code that was in `idle/`).

`wayland` and `compositors` are platform modules; `desktop` and `apps` have no violations.

## The 10 that remain

| Entries | Why it is there | What fixing it takes |
|---|---|---|
| 7 `config -> shell` | Config validation and the `helix-shell config` CLI call the bar widgets' settings schema (`widgetSettingSchema`, `validateWidgetSemantics`, `isBuiltInWidgetType`) and the desktop widget specs. Those come from the ~30 `shell/bar/widgets/*_widget_definition.h` files and the settings registry. | Move the widget definitions (data and validators) into a platform-level module, or split each definition into a schema part and a shell part. A startup callback would only hide the dependency, because `helix-service` validates widgets too. This is the main blocker for splitting the library. |
| 2 `theme -> ui/render` | `ThemeService` both computes palettes and drives a palette cross-fade (`AnimationManager`) and app-icon colouring. | Separate palette computation (platform) from applying it to the UI (shell). |
| 1 `wayland -> render` | `output_probe.cpp` creates a `LayerSurface` to probe outputs, and `compositors` also uses it. | Probe without a `LayerSurface`, or move the probe to the display layer and let `compositors` call it through an interface. |

## Splitting the library

`helix_core` is still one static library, so `helix-service` and the apps link the whole shell even though their
sources no longer include it. Once the three rows above are done, build base and platform as their own libraries and link
the independent processes against only those.
