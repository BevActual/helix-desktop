# Settings audit (2026-10-09)

Question asked of every Settings control: **does anything running in this session act on it?** The Tauri shell replaced the native
Helix shell, but Settings still listed the old shell's service settings. Checked by reading the consumers and, for theming, by changing
the setting on the live service and seeing which files changed.

| Page | Setting | Acted on by | Verdict |
|---|---|---|---|
| Appearance | `theme.mode` | helix-service theme pipeline: GTK, Qt, Warp, Brave themes, colour scheme | kept |
| Appearance | `theme.pure_black_dark` | same pipeline (generated files change when toggled) | kept |
| Accessibility | `accessibility.high_contrast` | helix-service sets the GTK high-contrast key | kept |
| Accessibility | `accessibility.ui_scale` | only the native shell | back, as the applications' interface scale (below) |
| Displays | `brightness.*` | the Tauri shell writes the backlight itself and ignores these | removed |
| Displays | `nightlight.*` | need a gamma provider; none exists in the Tauri shell (the live catalog has no rows) | removed |
| Sound | `audio.*` | feedback sounds and overdrive belonged to the native shell | page removed (output and input live in the control center) |
| Notifications | `notification.*` (12 rows) | the Tauri shell's own `org.freedesktop.Notifications` server reads none of them | replaced by a page of the two settings the shell honours (below) |
| Power & lock | `lockscreen.*`, `idle.*`, `battery.*` | helix-idle reads `~/.config/helix/idle.toml`, not these | replaced by a page that edits `idle.toml` |
| Dock | (all) | see settings-tauri.md | replaced earlier by `dock-settings.json` |

## What replaced them

* **Power & lock** edits `~/.config/helix/idle.toml` (lock after, screen off after, each can be switched off) through
  `helix_config::idle_prefs`; other keys in the file (the commands) are preserved, and helix-idle is restarted so it reads the file.
* **Displays** lists the connected outputs as the compositor reports them (read only).
* **Notifications** edits `~/.config/helix/notification-settings.json` through `helix_config::notification_prefs`: how long a banner stays when
  the application gave no time (2 to 15 s) and how many show at once (1 to 3). The shell watches the file and applies both to
  `helix_shell_model::notifications` with no restart. `tools/nested/settings-notifications-test.sh` and `notifications-prefs-test.sh`.
* **Interface scale** (Accessibility, 0.8 to 1.5) is followed by Settings, Software, System Monitor and Mobius: each watches the
  setting through `helix_desktop_api::Desktop::watch_ui_scale` and zooms its own windows. The shell's bar, dock and panels do not
  follow it yet. `tools/nested/ui-scale-test.sh`.
* **Dock** and **Wallpaper** are described elsewhere.

## Still to build, with their owners

* Night light needs a gamma-control client in the Rust shell (wlr-gamma-control), then rows for temperature and schedule.
* ~~A Sound page of real controls~~ — **Done.** Output and input device, volume and mute on the control center's audio model (`apps/helix-settings/web/src/pages/SoundPage.tsx`, `tools/nested/settings-sound-test.sh`). Still missing: a live input level meter.
* Notifications: banner time and count are done. Still open: a do-not-disturb schedule and per-application rules (each needs a consumer in `helix_shell_model::notifications` first).
* The shell's own chrome does not follow the interface scale: the bar and dock set layer-surface sizes and exclusive zones, so they need to follow the scale rather than be zoomed.
