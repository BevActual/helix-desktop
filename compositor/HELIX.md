# Helix compositor (helix-niri)

Helix’s compositor, at `compositor/` in the product tree. It is part of the root Cargo workspace (a niri fork): build it with `cargo build -p niri` from the repository root. Upstream [niri](https://github.com/niri-wm/niri) plus a `background-effect` **liquid-glass** child. The KDL child and its plumbing started as a port of [Niri-glass](https://github.com/zaroutt/Niri-glass); the lens model is Helix’s own (see below).

Product placement: [`docs/design/product-structure.md`](../docs/design/product-structure.md). Fedora + helix-niri is the sole Helix shell target.

The binary stays `niri` so it can replace a stock compositor binary on disk. Helix Shell detects this fork from `niri msg helix-capabilities` (`liquid_glass: true`) and falls back to the version string (`helix` after the niri version). Only then does it write `liquid-glass { … }` into `helix-blur.kdl`. Stock niri never sees that key.

The same reply carries `liquid_glass_params`: the `liquid-glass { }` child names this build parses (`LiquidGlassPart::PARAM_NAMES` in `niri-config/src/appearance.rs`). Helix Shell writes only keys from that list, so a newer shell never hands an older compositor a key that would fail the config load. Add every new `LiquidGlassPart` field to `PARAM_NAMES` in the same change.

## Trusted surface

A layer surface the person's shell uses for approvals (Helix's Agent Surface dialog) can be made *trusted*. Design and tests: `docs/design/trusted-surface.md` in helix-shell.

```kdl
trusted-surface {
    namespace "helix-tauri-confirmer"
    exe "/usr/local/bin/helix-shell-tauri"
    exe "/usr/bin/helix-shell-tauri"
    // rim-color "#ff9f0a"
    // allow-virtual-input   // let virtual pointers reach it (tests, accessibility tools); off by default
}
```

A surface is trusted only when it is on the Overlay layer, has that namespace **and** its client's executable (`/proc/<pid>/exe` from the connection's credentials) is listed. With no `exe` nothing is trusted and nothing changes. A trusted surface is:

- drawn above everything else on its output (a first render pass; the ordinary Overlay pass skips it) and first under the pointer (hit testing);
- given the keyboard before everything but the lock screen (`update_keyboard_focus`), so no other client's exclusive keyboard takes it;
- blanked from every capture target (`MappedLayer::block_out_from` forces `block-out-from "screen-capture"`);
- framed: the compositor draws a 4 px frame (`rim-color`, orange by default) along the edges of an output while a trusted surface is on it, above everything. No client can draw there, so a dialog without the frame is not the real one (`OutputState::trusted_rim`, the first elements of the render pass; it uses the layer map the render already holds, because locking it again deadlocks);
- closed to input that is not from a device: while one is mapped, virtual-pointer events and touch from client protocols are dropped (`Niri::blocks_synthetic_input`, a `physical_input` flag set per event in `process_input_event`).

Not covered, by design: `zwp_virtual_keyboard_v1` keys are delivered by smithay straight to the focused client and do not pass through niri, so the dialog itself refuses to be *allowed* by any key (a click produced by a key has `detail 0` and is ignored). `niri msg layers` shows `Trusted: yes` for such a surface and `niri msg helix-capabilities` reports `trusted-surfaces: true`; the shell uses these to know whether its dialog is protected.

Touch points when rebasing: `niri-config/src/misc.rs` (`TrustedSurface`, `Helix`), `niri-config/src/lib.rs` (field, `trusted-surface` / `helix` nodes), `src/layer/mapped.rs` (`trusted`, `block_out_from`), `src/handlers/layer_shell.rs` (decided when the surface maps), `src/niri.rs` (`layer_is_trusted`, `trusted_layer_on`, `blocks_synthetic_input`, `update_keyboard_focus`, `contents_under`, `layers_in_render_order`, the first render pass), `src/input/mod.rs` (`process_input_event`), `src/handlers/compositor.rs` (agent-desk placement on map), `src/helix/agent_desk.rs`, `niri-ipc/src/lib.rs` (`LayerSurface::trusted`, `HelixCapabilities`, `HelixAgentDesk`, `Workspace::is_agent_desk`), `src/ipc/{server,client}.rs`.

### Input settings

`HelixInput` reports and `HelixSetInput` changes the pointer and keyboard settings Settings offers (touchpad tap, natural scroll, speed, disable while typing, click method; mouse natural scroll, speed, acceleration profile, left-handed; keyboard repeat delay and rate, layout). They are overrides over `config.kdl`, remembered in `$XDG_STATE_HOME/helix/input-overrides.json`, applied at start and in `reload_config`, and live when set (`State::helix_set_input` repeats the side effects of a reload: repeat info, xkb, libinput devices). `HelixCapabilities.input_settings` is true. Touch points when rebasing: `src/helix/input.rs`, the two `apply_saved` calls (`src/main.rs`, `State::reload_config`), `src/ipc/server.rs`, `niri-ipc/src/lib.rs`.

### Summon

`helix-summon` (a bind action; `niri msg action helix-summon`) notes the title, app id and pid of the window that has the keyboard, then raises the window whose app id is `summon-app-id` (default `org.helix.Mobius`) or starts `summon-command` (default `helix-mobius`). `HelixSummonContext` returns the note once; it expires after a minute, and a summon while the summoned application is itself focused leaves no note. Config: `helix { summon-app-id "…"; summon-command "cmd" "arg"; }`. Touch points when rebasing: `src/helix/summon.rs`, `niri-config/src/{binds,misc}.rs`, `niri-ipc/src/lib.rs`, `src/input/mod.rs`, `src/ipc/server.rs`.

### Display profiles

The compositor remembers how the connected displays are set up (scale, orientation, resolution, position, on or off, variable refresh) for exactly this *set* of displays, in `$XDG_STATE_HOME/helix/display-profiles.json`, and applies it again when the set is connected. A display is known by `make model serial`, or by its connector when it reports no identity or shares one. IPC: `HelixSaveDisplayProfile`, `HelixApplyDisplayProfile`, `HelixForgetDisplayProfile`, `HelixDisplayProfile`; `HelixCapabilities.display_profiles` is true. Touch points when rebasing: `src/helix/display_profiles.rs`, `State::refresh_ipc_outputs` (the one call), `src/ipc/server.rs`, `niri-ipc/src/lib.rs`.

### Agent desk

```kdl
helix {
    agent-desk-name "Helix Agent"
}
```

IPC: `HelixPlaceOnAgentDesk`, `HelixFocusAgentDesk`, `HelixScreenshotOutput`, `HelixScreenshotAllOutputs`, `HelixAgentDesk` request. Workspace events include `is_agent_desk`. `HelixCapabilities` reports `agent_desk: true`.

After changing compositor IPC, rebuild (`cargo build --release -p niri --bin niri` from the repository root), run `tools/install-trusted-niri.sh apply`, and log out and back in. The session that is already running keeps its old `niri` binary until then; `desktop-capture all` and per-output capture need the installed build.

Colour tables: `wlr-gamma-control` is offered on a real (tty) session only. The headless test compositor offers a pretend 256-entry table when `HELIX_NESTED_GAMMA=1` and logs `HELIX gamma output=… r=… g=… b=…` (the last entry of each channel) or `reset`, so `helix-nightlight` can be tested end to end (`tools/nested/nightlight-test.sh`).

Nested tests (`tools/nested/run.sh`) set `HELIX_NESTED_HEADLESS=1` so niri starts with the headless backend and a 1280×720 output instead of winit on the host display.

## Rebase onto niri main

Keep `upstream` pointed at niri and rebase regularly:

```sh
git fetch upstream
git rebase upstream/main
```

Expect conflicts in:

- `src/render_helpers/liquid_glass.rs`
- `src/render_helpers/shaders/clipped_surface.frag` (liquid-glass block)
- `src/render_helpers/{background_effect,framebuffer_effect,xray}.rs` and `shaders/mod.rs`
- `src/utils/region.rs` (`TransformedRegion::glass_shapes`)
- `niri-config/src/appearance.rs` (`LiquidGlass` / `liquid-glass` child)
- `niri-config/src/debug.rs` (`liquid-glass-shader`)
- `src/utils/mod.rs` (`version()` includes `helix`)
- `niri-ipc` `HelixCapabilities` request/response and `niri msg helix-capabilities`
- `niri-ipc/src/lib.rs` (`HelixCapabilities`)
- `src/ipc/{server,client}.rs` and `src/cli.rs`

Do not vendor the Niri-glass overlay as a tree. Port the shader and KDL child onto current niri.

## Config

```kdl
layer-rule {
    match namespace="^noctalia-(bar-[^\"]+|notification|dock|panel|attached-panel|osd)$"
    background-effect {
        xray true
        liquid-glass {
            refraction-strength 1.0
        }
    }
}
```

`xray true` is required so the shader samples the scene behind the surface.

## Lens model

The glass is a slab. Its flat interior passes the backdrop through; its edge is a convex quarter-circle bezel, so the surface normal tilts outward toward the rim. The slab is three bezel-widths deep. A straight-on ray refracts at that tilt (Snell, `ior`) and lands on the backdrop just outside the shape: the rim shows a compressed band of what lies beyond the edge, in real pixels. Red and blue disperse a little (`fringing`). A light at `light-angle` puts a specular on the rim facing it, a fainter one on the far wall, a one-pixel hairline all the way round, and an inner shadow where the rim turns away (`edge-shadow`).

| child | range | default | meaning |
| --- | --- | --- | --- |
| `refraction-strength` | 0..4 | 1 | lens displacement scale; 1 is physical for `ior`, 0 keeps only the rim lighting |
| `bezel-width` | 0..400 px | 0 | bezel width; 0 derives it as 1.25 × corner radius (clamped 3..48 px), so near-square shapes get a hairline rim and capsules a full lens |
| `ior` | 1..3 | 1.5 | index of refraction |
| `fringing` | 0..1 | 0.35 | chromatic dispersion at the rim |
| `specular` | 0..2 | 1 | rim highlight and hairline intensity |
| `light-angle` | -180..180° | -35 | light direction, clockwise from the top of the screen |
| `edge-shadow` | 0..1 | 0.5 | inner shadow opposite the light |
| `edge-padding` | 0..1 | 0 | grow the effect area by this fraction of the shorter side |

Helix Shell maps its Glass slider straight onto `refraction-strength` (0..1) and leaves the rest at defaults.

### One lens per shape

A layer surface that sets a blur region gets one lens per shape rather than one for the whole surface. Helix Shell tessellates each rounded shape into horizontal strips (a top band of `ceil(r)` rows narrowing toward the top, one full-width body, a bottom band). `TransformedRegion::glass_shapes` groups touching strips, takes the union’s bounding box as the shape, and reads the top and bottom radii from the narrow bands. A square side (an attached panel meeting the bar) yields a zero radius there. Each shape renders with its own SDF and anti-aliased rounded clip, so a bar capsule, a panel, and a toast on one surface each carry their own bezel. Surfaces without a blur region still get a single lens over the layer geometry.

### Live tuning

```kdl
debug {
    liquid-glass-shader "/path/to/compositor/src/render_helpers/shaders/clipped_surface.frag"
}
```

recompiles the fragment shader from that file on every config load (touch `config.kdl` to reload). A shader that fails to compile logs a warning and leaves the previous program in place. Remove the key for release.

## Look settings

`HelixLook` reports and `HelixSetLook` changes animations (on or off), `window_gaps` (0 to 64), `cursor_size` (16 to 64) and `focus_color` (`#rrggbb`, which recolours the focus ring and the window border). Only the fields that are set change. They are overrides, like `HelixInput`: kept in `$XDG_STATE_HOME/helix/look-overrides.json`, applied over `config.kdl` at start (`main.rs`) and in `reload_config`, and applied at once when set (`src/helix/look.rs`). Advertised as `look_settings` in `HelixCapabilities`.
