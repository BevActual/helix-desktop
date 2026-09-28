# Helix Human Interface Guidelines

How Helix looks and behaves, for every surface the shell draws: bar, dock, menus, launcher,
Control Center, notifications, OSD, lock screen, and desktop widgets. When a change touches how
something looks, it should point back to a rule here. When a rule here is wrong, change the rule
first.

Helix takes its structure from Apple's Human Interface Guidelines and its identity from two
things of its own: wallpaper-derived color and the liquid-glass material helix-niri draws. It
does not copy Apple's visuals.

## Principles

1. **Calm by default.** One accent, neutral surfaces, few borders. Color means something:
   selection, state, or data. Nothing is colored just to be colorful.
2. **Depth comes from material, not decoration.** Glass says "this floats above your work".
   Solid says "this is content". Shadows and outlines support that, never replace it.
3. **Hierarchy you can read at a glance.** Selected beats hovered beats resting. Primary
   actions beat secondary ones. Values beat labels.
4. **One way to do each thing.** One menu, one list row, one card, one focus ring. A second
   version of a component needs a reason written here.
5. **Accessible as a baseline.** High Contrast, Reduced Motion, and keyboard navigation work
   everywhere, not just where someone remembered.

## Materials and tiers

Liquid Glass is a hierarchy, not a skin. Every surface belongs to exactly one tier.

| Tier | What it is | Material | Helpers |
|------|------------|----------|---------|
| **Chrome** | The frame that floats over your work: panels, sidebars, headers, menus | Glass per `transparency_mode` (Solid, Soft = regular glass, Glass = clear glass). Clear glass always gets a dimming layer. | Panel chrome, `popup_chrome` |
| **Leftover** | Content sitting on chrome: cards, catalogs, grouped lists | Solid wells. Color and type live here. | `applySectionCardStyle`, `applyContentCardStyle`, `applyGroupedListStyle`, `applyGroupedListRowStyle` |
| **Modules** | Functional plates you operate: session actions, quick toggles, sound and brightness | Frosted plate, not a second glass layer | `applyModuleWellStyle` |
| **Widgets** | Glanceable information that deep-links somewhere: clock, weather, media | Stands alone; not a mini settings page | Desktop and Home widgets |

Rules:

- **Never glass on glass.** Content on a glass panel is leftover (solid), not another glass card.
- **One compact card per tab**, and the header carries the title; do not repeat it.
- **Widgets deep-link**, they don't grow controls. Click the clock, get Calendar.

### Where each surface sits

| Surface | Tier | Notes |
|---------|------|-------|
| Bar | Chrome | Widgets on it are glyphs and short values, not cards. |
| Dock | Chrome | Items are icons; running state is a dot, not a fill. |
| Menus and popovers (context, dock, tray, dropdowns) | Chrome | One component (see Menus). |
| Launcher | Chrome frame, leftover results | Results are grouped-list rows. |
| Control Center, Settings | Chrome frame, leftover pages | Home uses modules for quick controls. |
| Notifications (toasts and history) | Leftover on chrome | Actions are buttons, not links. |
| OSD (volume, brightness) | Module | Value first, glyph second. |
| Lock screen and greeter | Chrome over a dimmed wallpaper | Clock is a widget. Must match helix-greeter. |
| Desktop widgets | Widgets | Glanceable, deep-link. |

## Color

Colors come from the palette (`src/ui/palette.h`); use roles, never literal colors, outside
`src/theme/`.

- **One accent.** `Primary` is the only accent: selection, the checked state, progress, and
  the keyboard focus ring (`focusRingColorSpec()`). `OnPrimary` sits on it.
- **Hover is a state, not a hue.** `Hover` resolves to the surface tinted toward the accent
  (`surface_variant` mixed with `primary`, 22% dark and 16% light) and `OnHover` to normal
  text. This gives one visible order, selected (full accent) > hovered (tint) > resting,
  in a single hue. The palette's authored `hover` value is still exported to app templates
  unchanged; only the shell's own roles are derived. See `uiHover()` in `src/ui/palette.cpp`.
- **Hover borders use the accent.** A thin line in the hover tint disappears, so toggles,
  sliders, and selects outline in `Primary` on hover.
- **Secondary and tertiary are for meaning, not interaction.** Charts, categories, badges,
  and status. They never mark hover or selection.
- **Error is for errors.** Destructive actions use `Error`, and only at the point of no return.
- **Surfaces are neutral.** `Surface` for chrome backing, `SurfaceVariant` for wells.
  `Outline` only where two surfaces need separating and material alone doesn't do it.

## Shape

- Radii come from `Style::radius*`: `Sm` 3, `Md` 6, `Lg` 9, `Xl` 12, `2xl` 18, `3xl` 24.
- **Concentric corners.** An inner radius is the outer radius minus the padding between them.
  Leftover wells use `radius3xl`; grouped lists inside them `radius2xl`.
- Spacing comes from `Style::space*`: `Xs` 4, `Sm` 8, `Md` 12, `Lg` 16. No raw numbers in
  `setGap`, `setPadding`, or `setRadius`.
- Control heights: `controlHeightSm` 32, `controlHeight` 38, `controlHeightLg` 44.

## Type

Five sizes from `Style::fontSize*`: `Mini` 11, `Caption` 13, `Body` 14, `Title` 16, `Header` 20.

- **Body** for almost everything, including well and group headers (bold).
- **Caption** for secondary lines under a row title.
- **Title** is for values (a percentage, a time, a track name), not labels.
- **Header** for a page or window title, once per surface.
- **Mini** only where space is genuinely tight (badges, dense bar widgets).

## Motion

Durations from `Style::anim*`: `Fast` 100 ms (hover, press), `Normal` 200 ms (open, close,
reveal), `Slow` 400 ms (large layout changes), `Spring` 320 ms. Motion explains where
something came from; it never delays a result. With Motion off (Reduced Motion), transitions
become cross-fades or are skipped.

## Menus

Every menu, whether context, dock, tray, or dropdown, should look and behave the same: chrome
material, `Body` rows, hover tint, `Primary` for the checked item, separators between groups,
Escape to close, arrow keys to move, and closing whatever other menu was open. Opening a
menu must never take an xdg_popup grab that isn't on the topmost grabbing popup (see
`PopupSurface::wireGrab`).

## Settings structure

Settings is organized like macOS System Settings: a flat sidebar of named tabs in unlabeled
clusters, settings shown in context, and sub-tabs only where one tab has several facets.

- **More main tabs, not hidden hubs.** No "General" catch-all; a thing gets its own sidebar
  tab. Clusters are separated by dividers, not captions.
- **Sub-tabs for facets of one concept** (Privacy & Security → Privacy | Firewall |
  Security; Desktop & Dock → Bar | Dock | Widgets ...), as chips under the tab title. Never
  to merge unrelated things (Sound and Displays stay separate). A multi-page tab is a
  `HubTab` (`src/shell/control_center/hub_tab.h`); pages inside it are ordinary tabs.
- **Old ids keep working.** A page's id stays a valid open context for IPC, Home links, the
  launcher's Panels provider, and search; `kHubChildren` in `control_center_panel.cpp` maps it
  to its tab and sub-tab.
- **Live "app" pages** (Calendar, Weather, Media, Capture, Clipboard, AI, Templates, plugin
  tabs) sit in the Apps cluster near the bottom, like third-party panes on macOS.
- **Clusters** (`TabGroup`, `kBuiltinTabs`): connectivity; attention; system; desktop;
  devices and input; sharing; apps; advanced (hooks, compositor).
- **Search** finds individual settings and opens the tab and sub-tab that shows them
  (`tabIdForSettingsGroup`).
- **Sub-panes, macOS style.** A pane's extra pages open from navigation rows (glyph, title,
  chevron) and replace the pane in place, with a back header; Cancel (Escape) goes back, and
  the pane reopens on its main page. A sub-pane can be a live page (a device list) or a
  settings-only page of toggles and sliders (`SettingsPane::createInline` for one group).
  Helpers: `makeNavRow` and `makeSubPaneHeader` in `tab.h`. Sound, Displays, and Notifications are the references; a tab with several settings groups gives each group its own sub-pane (one `SettingsPane` per group).
- **Chip sub-tabs only for peers you switch between often** and that share one purpose
  (Privacy & Security today); prefer sub-panes otherwise.
- **Settings in the pane.** A tab with a single page shows its settings under the live
  content, and the page scrolls as one list (`SettingsPaneOptions::inlineSettings`). A
  hosted page with one settings group drops the group heading, since the tab or sub-tab
  already names it. App-like tabs (Weather, Calendar, Media) keep a separate settings view
  behind a labeled **Settings** / **Done** header button.

## Accessibility

- **High Contrast** maps to Increased Contrast and Reduced Transparency: palette stretch,
  opaque chrome and leftover, contrasting outlines, no module softness.
- **Motion off** maps to Reduced Motion.
- **Keyboard:** every interactive element is reachable, shows a focus ring, and responds to
  Enter/Space; every popup closes on Escape.
- **Contrast:** text on any fill meets WCAG AA (4.5:1 body, 3:1 large text and glyphs).

## Catalog tabs

Theme palettes, Type, Language, Icons, Wallpaper, and Templates use Wallpaper navigation: icon
sources, search, and the current pick sit on the compact card; leftover is only the grid.
Click sources, don't hover-preview catalogs. When the window grows, catalogs add columns
instead of stretching tiles.

## Sibling repos

Helix is three repos and one product: helix-shell (this repo), helix-niri (compositor; draws
liquid glass, reports `niri msg helix-capabilities`), and helix-greeter (login; syncs palette,
wallpaper, `corner_radius_scale`, `glass`, `refraction`, and font). A change to a shared
contract lands in all three in the same piece of work.

## Known gaps

What doesn't follow these rules yet, roughly in order:

1. **Three menu implementations:** `ContextMenuPopup`, the dock's `DockPopup`, and the tray's
   `TrayMenu`. Dock and tray should move onto `ContextMenuPopup`.
2. **Tier pass on each surface** in the table above that predates this document: OSD,
   notifications, dock, bar.
3. **Most control tabs still keep settings behind the Settings button.** Sound, Displays,
   and Notifications use sub-panes so far; Power is next. Hub sub-tabs can't be hidden individually with
   `hidden_tabs` yet, and there is no account card at the top of the sidebar.
4. About 45 raw radius and spacing numbers and about 50 literal colors remain in `src/shell`
   and `src/ui`.
