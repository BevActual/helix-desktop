# The compositor is ours: where Helix should be built into it

Status: audit and first slice, 2026-10-21. Direction from the person: Helix is not obliged to follow upstream niri. The compositor (`compositor/`, in the workspace, the only one in use; it reports itself as "niri 26.04 helix") can change wherever integration makes the product better, and the same freedom applies to the other components. This note lists where Helix works *around* the compositor today, what each workaround costs, and what the better shape is. It is ordered by value.

## What is native already

* **Trusted surface** (`trusted-surface.md`): the approval dialog is drawn above everything, takes the keyboard, is blanked from captures, and is framed by the compositor itself.
* **Agent desk**: a workspace the compositor knows is the agent's (`HelixAgentDesk`, `HelixPlaceOnAgentDesk`, `is_agent_desk` on workspaces).
* **Capture**: per-output and all-output screenshots over IPC (`HelixScreenshotOutput`).
* **Liquid glass**: the background effect and its parameters, reported by `HelixCapabilities`.

## Where Helix works around it

| # | Today | Cost | Better shape |
|---|---|---|---|
| 1 | **Displays were saved by rewriting KDL.** Settings changed an output over IPC (session only), then edited `~/.config/niri/helix-setup.kdl`. | A text editor in another process: brace matching, one-line blocks, preserving hand edits; outputs keyed by connector, which changes with the port. | **Done.** The compositor remembers the setup of the connected displays *per set of displays* (`$XDG_STATE_HOME/helix/display-profiles.json`; a display is known by make, model and serial, or its connector when it has no identity) and applies it again when that set is connected (`src/helix/display_profiles.rs`, hooked in `refresh_ipc_outputs`). IPC: `HelixSaveDisplayProfile`, `HelixApplyDisplayProfile`, `HelixForgetDisplayProfile`, `HelixDisplayProfile`; `HelixCapabilities.display_profiles`. Settings sends one save request after each change and no longer writes KDL when the compositor can do it (it still does, by identity, for a compositor that cannot). The person's `config.kdl` stays the baseline and is never rewritten. A laptop alone and the same laptop docked are two remembered layouts. |
| 2 | **Wallpaper is a separate process** painting a layer surface from a TOML file. | A process per screen set, a lag at login, nothing shared with blur/glass, nothing for a lock screen or the greeter to reuse. | Keep the process for now; the compositor-drawn background becomes worth it when glass needs to sample the wallpaper, or the greeter and lock screen must match it exactly. |
| 3 | **Night light** had no owner. | A missing feature. | Built as a client of the compositor's `wlr-gamma-control` (this note, below). |
| 4 | **Summon Mobius from anywhere.** | A key that spawns a program cannot know what was focused: by the time the window exists, it has the keyboard. | **Done.** `helix-summon` is a compositor action (bind it in `config.kdl`, e.g. `Mod+Shift+A { helix-summon; }`). At the key press it notes the focused window's title, app id and pid, then raises the Mobius window if one is open (`helix { summon-app-id … }`, default `org.helix.Mobius`) or starts it (`summon-command`, default `helix-mobius`). `HelixSummonContext` hands the note over once and it expires after a minute. Mobius picks it up when it gains focus, shows "About: <title> · sent with your message ×" above the composer, and sends a two-line note with the next message only if the person leaves the chip. No selection or window contents are read. |
| 5 | **Notifications and the shade** are drawn by the Tauri shell as layer surfaces. | Fine today; the compositor cannot, for instance, keep banners out of screen captures or above a fullscreen game by itself. | Mark the notification namespace with the trusted-surface machinery's capture block, not a new surface kind. |

## First slice: night light

`wlr-gamma-control` is the standard way to tint a screen, and the compositor already implements it for real displays (a tty session only). So night light is a small client rather than a compositor change:

* `core/helix-nightlight-model` (pure): the settings (`~/.config/helix/nightlight.json`), when the screen should be warm (always, or a window of hours with a 30-minute fade at each end, through midnight if it wraps), and the colour ramp for a temperature (a blackbody approximation scaled so 6500 K is exactly neutral).
* `services/helix-nightlight` (a systemd user unit like `helix-idle`): reads the file (reloading when it changes), asks the clock once a second, holds each output's gamma control **only while the screen is meant to be warm** and lets go otherwise, so the compositor restores the true colours and another tool can have the tables. It sends a table when the warmth changes, not every tick; an output that refuses is left alone and asked again after 30 s; a compositor with no gamma support (a nested window) makes it log and exit cleanly.
* Settings, Displays: a Night light group at the top (switch, warmth, schedule, From and Until).
* **A compositor change for testing**: the headless test compositor offers a pretend 256-entry table when `HELIX_NESTED_GAMMA=1` and logs what a client sets, so `tools/nested/nightlight-test.sh` checks the whole path: file → service → protocol → table (full red, less green, least blue) → released when turned off. That test found a real bug (the descriptor's position must be rewound to the start before it is sent; the compositor reads from it).
* Not verified: the colours on a real screen. Do that on the real session after `tools/install.sh` and `systemctl --user enable --now helix-nightlight`.

## Next, in order

1. A night-light **schedule from sunrise and sunset** once there is a location (the Quick Settings tile is done).

## Summon, as built

* Compositor: `src/helix/summon.rs`, the `HelixSummon` action (`helix-summon` in KDL, `niri msg action helix-summon`), `Request::HelixSummonContext`, `HelixCapabilities.summon`, and `summon-app-id` and `summon-command` in the `helix { }` block.
* Mobius: the `summon_context` command, the chip in `chat.js`, and the note attached by `attachmentPayload()` (named "the window they were using").
* A summon while Mobius itself has the keyboard raises it and leaves no note, so Mobius never becomes the subject of its own question.
* Tested end to end in `tools/nested/mobius-summon-test.sh` (a second window has the keyboard, the action is sent, Mobius is raised and not started twice, the chip appears, the note goes with the next message once, a summon from Mobius leaves no chip). Not done: reading the selected text (the primary selection) as a second, removable chip; that is the next step if it proves useful.

## Display profiles, as built

* A profile is `{displays: [sorted names], outputs: [{name, off, scale, transform, mode (w, h, mHz), position, vrr}]}`; at most 32 are kept. "Applied again" means the same actions the IPC `Output` request performs (the compositor's own `modify_output_config`, then one `reload_output_config`), so it cannot do anything the person could not do by hand.
* The profile of a set is applied once when the set changes, not on every refresh, and saving marks the current set as already applied. Outputs appear one at a time at start-up, so a partial set simply has no profile until the full one is connected.
* Checked end to end in `tools/nested/settings-displays-test.sh`: a change in Settings is remembered, no KDL is written, and `HelixApplyDisplayProfile` after putting the display back by hand restores it. Not checked: a real hotplug on hardware.
* Found on the way: `helix_capabilities()` and `agent_desk_status()` in `core/helix-compositor` read the reply as if the fields were at the top level, but the reply wraps them (`{"HelixCapabilities": {…}}`), so both always returned their defaults and every check built on them (`agent_desk`, `liquid_glass`) read false. Fixed, with a test.
