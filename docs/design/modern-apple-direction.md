# Helix, as if modern Apple built it

The bar for every surface: very modern and sleek, the way macOS Tahoe and iOS 26 feel. Not a copy of Apple's platform conventions (Helix
keeps what is better, see `apple-like.md`), but Apple's standard of finish: restraint, depth, motion that feels physical, and nothing that looks
like a raw web form. This note says how Apple would refine Helix, what is done, and what comes next.

## Principles

1. **One material.** Every panel, sheet, bar, dock, menu and toast is the same glass: the panel colour under a soft sheen, a lit top edge
   (rim), a hairline, and a deep soft shadow. The compositor's blur and refraction sit behind it. No surface invents its own.
2. **Radii nest.** An inner shape's radius plus its padding is the outer radius (28 panel, 20 module, 14 control, capsule for buttons).
   Controls are round: capsule buttons, circular toggles and toolbar buttons, capsule pop-ups and sliders.
3. **Colour means "on".** Surfaces stay neutral; the accent appears only where something is on or chosen (a toggle's circle, the selected
   row, the primary button). Icon tiles carry identity colour (System Settings tiles, model families), not state.
4. **Hierarchy by type, not boxes.** Large titles (30/700) on pages, a small title in a glass toolbar once scrolled, section headers in Title
   Case, row labels in sentence case, secondary text in the second label colour.
5. **Progressive disclosure.** A row shows the value; detail opens in a sheet; destructive and rare actions live in a ⋯ menu, confirmed in
   an alert. Every page opens at its top with a header card saying what it is for.
6. **Motion is springy and short.** Things grow from where they came from with a damped spring (about 320 ms, no overshoot for panels, a
   little for small things like toasts and the dock). Reduce Motion turns it all to fades.
7. **Show, then act.** Status first (an orb, a gauge, a ring), the action beside it. Progress is a ring you can press to stop.
8. **Accessibility is part of the look.** Glass strength is a setting (Reduce Transparency), contrast holds in light and dark, everything is
   reachable by keyboard, every control has a name.

## Done (2026-10-29)

* Kit (`core/shared-ui`): `body.hx-modern` layer: floating glass sidebar with tinted selection, capsule controls, floating sheets, toolbar
  buttons, widgets, status hero, tinted cards, `ActionMenu`, `ProgressRing`, labelled field rows, status dots.
* Helix Models: dashboard Overview, featured "Recommended for this computer", App Store catalogue, product-style Get sheet, rings for downloads.
* Helix Software: on the modern layer.
* Settings: coloured page tiles, a header card on every page, glass toolbar title, tiled list icons, capsule segments, the red window dot.
* Shell: Liquid-glass panels with rim and sheen; control centre as modules (round toggles that fill with the accent when on, tall capsule
  sliders, round system buttons); Spotlight-style launcher; glass toasts with a spring; dock with a lit rim and springy icons; capsule OSD.
* Mobius: floating glass sidebar with a coloured tile per workspace (a rail of tiles when a stage is open), tinted selection, glass
  composer capsule, start cards with tiles, capsule starters, glass title bar.
* System Monitor: the Overview is widgets (CPU and memory as wide widgets with the last minute as a filled area, swap ring, network, load,
  graphics), coloured page tiles, large titles.
* Login (greeter): no card; a large bold clock high up, then a lit avatar, the name and a glass password capsule on the wallpaper; glass
  restart and shut down. High contrast and solid glass keep the card.
* The shell's desktop layer is scoped to `body.hx-shell.hx-modern` so its generic names (`.panel`, `.tile`, `.row`) never reach an app.

## Next, in order

1. **Control centre, deeper**: a connectivity module (Wi-Fi, Bluetooth, VPN as circles in one module), a media module with artwork, slider icons
   inside the fill for brightness too, modules the person can rearrange.
2. **Menu bar**: an option for a fully transparent bar over the wallpaper (Tahoe), with only grouped items in glass.
3. **Lock screen** (drawn natively): match the login's bold clock and lit avatar.
4. **App icons**: one squircle grid and shading for Helix's own icons so the dock and launcher look like one family.
5. **Sound and feel**: short, quiet UI sounds (off by default), and a consistent focus ring.
6. Make `hx-modern` the default for every Helix surface and delete the older styles it replaces.
