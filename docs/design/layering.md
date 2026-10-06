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

`desktop` (the desktop service) and `apps` (Settings, Software, System Monitor) are **independent processes**. They may
include base, platform and each other, never display or shell, so they can run without the shell. They currently have
no violations. `desktop_api` is the client library for the service (protocol, client, display text); every process uses it.

## The ratchet

- A **new** violation fails `layering`. Fix the dependency: move the shared piece down a layer, or pass it in.
- A baseline entry that no longer exists also fails, so a fix must remove its line
  (`tools/check-layers.py . --update-baseline` rewrites the file). The list can only shrink.
- `tools/check-layers.py . --list` groups the remaining violations by module pair.

## What has moved so far

`desktop_api/` (client library), `core/poll_source.h` and `core/color.h` (27 modules reached up for them),
`theme/color_roles.h`, `core/motion_durations.h`, `net/wallhaven_client.*`, `config/widget_gesture.*`,
`config/widget_action.*` and `config/launcher_providers.h`. The baseline went 106 -> 37.

## Remaining work, in the order that pays off

1. **config -> shell (registries).** `widget_settings_registry` lists every bar widget definition, so config validation
   reaches into the shell. Invert it: the shell registers its widget schema with config at startup. Needs care around
   `noctalia config validate`, which runs without a shell.
2. **wayland/surface.* and text_input_service.** Rendering glue inside a platform module. Split the connection and
   protocol code from the surface glue so wayland can be built without render and ui.
3. **theme -> ui/render, idle -> ui/render, config -> ui.** Mostly `ui/palette.h` and `ui/style.h` constants and types.
   Same approach as `theme/color_roles.h`: move the plain data down, keep the drawing types where they are.
4. **Split the library.** `noctalia_core` is one static library, so `helix-service` and the apps still link the whole
   shell. Once the platform layer has no upward includes, build base and platform as their own libraries and link
   the independent processes against those only.
