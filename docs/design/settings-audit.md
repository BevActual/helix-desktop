# Settings audit (2026-10-09)

Question asked of every Settings control: **does anything running in this session act on it?** The Tauri shell replaced the native
Helix shell, but Settings still listed the old shell's service settings. Checked by reading the consumers and, for theming, by changing
the setting on the live service and seeing which files changed.

| Page | Setting | Acted on by | Verdict |
|---|---|---|---|
| Appearance | `theme.mode` | helix-service theme pipeline: GTK, Qt, Warp, Brave themes, colour scheme | kept |
| Appearance | `theme.pure_black_dark` | same pipeline (generated files change when toggled) | kept |
| Accessibility | `accessibility.high_contrast` | helix-service sets the GTK high-contrast key | kept |
| Accessibility | `accessibility.ui_scale` | only the native shell | removed |
| Displays | `brightness.*` | the Tauri shell writes the backlight itself and ignores these | removed |
| Displays | `nightlight.*` | need a gamma provider; none exists in the Tauri shell (the live catalog has no rows) | removed |
| Sound | `audio.*` | feedback sounds and overdrive belonged to the native shell | page removed (output and input live in the control center) |
| Notifications | `notification.*` (12 rows) | the Tauri shell's own `org.freedesktop.Notifications` server reads none of them | page removed |
| Power & lock | `lockscreen.*`, `idle.*`, `battery.*` | helix-idle reads `~/.config/helix/idle.toml`, not these | replaced by a page that edits `idle.toml` |
| Dock | (all) | see settings-tauri.md | replaced earlier by `dock-settings.json` |

## What replaced them

* **Power & lock** edits `~/.config/helix/idle.toml` (lock after, screen off after, each can be switched off) through
  `helix_shell_core::idle_prefs`; other keys in the file (the commands) are preserved, and helix-idle is restarted so it reads the file.
* **Displays** lists the connected outputs as the compositor reports them (read only).
* **Dock** and **Wallpaper** are described elsewhere.

## Still to build, with their owners

* Night light needs a gamma-control client in the Rust shell (wlr-gamma-control), then rows for temperature and schedule.
* ~~A Sound page of real controls~~ — **Done.** Output and input device, volume and mute on the control center's audio model (`apps/helix-settings/web/src/pages/SoundPage.tsx`, `tools/nested/settings-sound-test.sh`). Still missing: a live input level meter.
* Notification behaviour (how many toasts, where, how long) belongs to `helix_shell_core::notifications`; nothing there is configurable yet.
* Shell-level accessibility (text size) has no consumer; it needs a scale applied by the shell and by Settings.
