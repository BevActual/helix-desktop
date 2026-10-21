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
* **Displays** lists each connected output and sets its resolution (from the modes the monitor advertised), scale (100% to 200%), and orientation. With two or more displays it also sets arrangement and can turn an extra display off. Variable refresh appears only when the monitor supports it. The compositor applies the change immediately, and Settings records it in `~/.config/niri/helix-setup.kdl` (included from the compositor config). `tools/nested/settings-displays-test.sh`. **Night light** sits at the top of the page (switch, warmth in kelvin, an optional schedule with From and Until hours, a half-hour fade at each end). It writes `~/.config/helix/nightlight.json`, which the `helix-nightlight` service reads (see `compositor-integration.md`). On the Helix compositor Settings sends one request after each change and the compositor remembers the whole layout per set of connected displays (`compositor-integration.md`); no KDL is written. On another compositor Settings writes only the changed display to `helix-setup.kdl`, by its make-model-serial identity, and pins a resolution or position only when that was the thing changed. A "Remembered layout" row says whether this set is saved.
* **Wallpaper** sets one picture for every display, or a picture for one connected display. The choice is `~/.config/helix/wallpaper.toml`: `[wallpaper.default]` for every screen, `[wallpaper.monitors.<connector>]` for one screen. `helix-wallpaper` already paints from that file and reloads it. `tools/nested/settings-wallpaper-test.sh` checks the file; `wallpaper-test.sh` checks the pixels.
* **Notifications** edits `~/.config/helix/notification-settings.json` through `helix_config::notification_prefs`: Do not disturb, quiet hours (a from/until window on the 24-hour clock, including through midnight), how long a banner stays when
  the application gave no time (2 to 15 s), and how many show at once (1 to 3). The shell watches the file and applies these to
  `helix_shell_model::notifications` with no restart. Quiet hours are checked again when the hour changes. Urgent banners still show, and the Do not disturb switch stays independent. Quick Settings writes the same Do not disturb value, so the two switches stay together and it survives a restart. `tools/nested/settings-notifications-test.sh` and `notifications-prefs-test.sh`.
* **Interface scale** (Accessibility, 0.8 to 1.5) is followed by Settings, Software, System Monitor and Mobius: each watches the
  setting through `helix_desktop_api::Desktop::watch_ui_scale` and zooms its own windows. The shell's bar, dock and panels do not
  follow it yet. `tools/nested/ui-scale-test.sh`.
* **Appearance mode** (dark, light, automatic) is followed by Settings, Software, System Monitor and Mobius through
  `helix_desktop_api::Desktop::watch_theme_dark`: `auto` is resolved with the same day and night schedule the service uses and
  re-checked each minute. They set `data-theme` on the page, and again after each page load. `tools/nested/ui-theme-test.sh`.
* **Dock** and **Wallpaper** are described elsewhere.

## Still to build, with their owners

* ~~Night light~~ — **Done**: `services/helix-nightlight` (a `wlr-gamma-control` client), `core/helix-nightlight-model`, and the rows on Displays. The **Quick Settings** Night Light tile reads and writes the same file (`helix_nightlight_model::{load, set_enabled}`; it is no longer a setting of the desktop service), so the tile, the Settings page, the Act/agent command and the key binding agree. Still to do: a schedule from sunrise and sunset (needs a location).
* ~~A Sound page of real controls~~ — **Done.** Output and input device, volume and mute, and a microphone test (`Test microphone` on the Sound page). The test calls `Audio1.StartInputMeter` for the microphone in use, shows `InputLevel` while it is on, renews the eight-second lease, and calls `StopInputMeter` on leaving the page. `apps/helix-settings/web/src/pages/SoundPage.tsx`, `tools/nested/settings-sound-test.sh`.
* Notifications: banner time, banner count, the Do not disturb switch and quiet hours are done. Still open: per-application rules (they need a consumer in `helix_shell_model::notifications` first).
* The shell's own chrome does not follow the interface scale: the bar and dock set layer-surface sizes and exclusive zones, so they need to follow the scale rather than be zoomed.
