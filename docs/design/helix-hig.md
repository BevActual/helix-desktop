# Helix Human Interface Guidelines

How Helix looks and behaves, for every surface the shell draws: bar, dock, menus, launcher,
Control Center, notifications, OSD, lock screen, and desktop widgets. When a change touches how
something looks, it should point back to a rule here. When a rule here is wrong, change the rule
first.

Helix uses GTK4/libadwaita as its shared control and application design baseline. Apple-inspired
craftsmanship means restrained presentation, clear navigation and polished behavior within native
libadwaita conventions. Applications remain adaptive and portable across Linux desktops and mobile
Linux devices. The custom shell renderer follows the same visual grammar and targets helix-niri only.

## Shared application and shell contract

- Prefer standard GTK/libadwaita controls, navigation, focus states and accessible geometry.
- Application styling is limited to semantic colors and branding. Do not replace native control shapes.
- Use images, previews and data charts to make applications more graphical where they help users
  understand or choose. These are application content; controls and navigation remain native.
  Keep textual equivalents for chart values, native high-contrast colors and responsive layouts.
- Shell controls approximate the same geometry and behavior with the existing custom renderer.
- Glass is reserved for floating shell chrome. Application content and control groups are solid.
- Desktop windows use adaptive navigation: sidebar and content become a single-page flow below 600sp.
- Essential actions are visible without hover. Touch, keyboard, pointer, portrait and on-screen keyboards
  are first-class inputs. Validate 360, 768 and 1280 logical-pixel widths.
- Helix appearance tokens map to toolkit-supported roles. Native appearance is the fallback when Helix
  services are unavailable. System high contrast takes precedence over custom application colors.
- Compositor capabilities are explicit. Applications run without the shell or helix-niri; desktop-only
  pages are omitted when their capabilities are absent.

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
| **Modules** | Functional plates you operate: session actions, quick toggles, sound and brightness | Solid native-style control group | `applyModuleWellStyle` |
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
| Control Center | Floating chrome, solid controls | Quick session actions. |
| Settings and applications | Native libadwaita chrome and solid content | Adaptive persistent windows. |
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
  Control Center wells and grouped lists use `radiusXl`; avoid oversized nested rounding.
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

The standalone Settings application uses standard libadwaita preference groups and adaptive sidebar
navigation. The independent desktop service owns settings persistence and migrated device backends.
The old shell Settings editors were retired by explicit user direction, including advanced editors
whose native replacements are unfinished. Preserve existing configuration and show clear unavailable
states for those editors. Remaining live tool pages continue during migration; full desktop cutover
still requires every inventory entry to have a working destination. No transitional legacy tools
window is introduced. The following shell rules apply to the remaining live tool pages.

Native Desktop & Dock places visible section-navigation buttons before its layout preview and
settings groups. These use standard GTK buttons in an adaptive flow layout, move keyboard focus
to the destination controls, and retain the full scrollable page. Unfinished editor notices follow
the working settings, rather than interrupting everyday configuration.

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
  the pane reopens on its main page. Keyboard focus moves to Back on entry and returns to
  the originating row when possible. A sub-pane can be a live page (a device list) or a
  settings-only page of toggles and sliders (`SettingsPane::createInline` for one group).
  Helpers: `makeNavRow` and `makeSubPaneHeader` in `tab.h`. Sound, Displays, Notifications, Power, Bluetooth, Network, and System are the references. Related settings groups can share one sub-pane when one group's switch controls the others, as in System → Monitoring.
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

## Everyday controls and connection details

Home is the quick Control Center. Keep the control name stable and show its current state
on a secondary line: a connected network name must not replace the Wi-Fi label. The glyph
button toggles the feature, and its adjacent chevron opens the related native
Settings page for Wi-Fi, Bluetooth, and VPN. If Settings is unavailable, the
chevron opens the existing Control Center details. Navigation to Settings closes
Control Center. Shell-only controls continue to open their shell details.
Apply the active accent to the toggle, keeping the surrounding labels and surface neutral.
Sound and brightness have visible labels as well as glyphs and values.

Home offers labeled Lock, Suspend, and Power actions. Power opens the full session controls;
restart, logout, and shutdown do not compete with everyday controls on Home.
Capture and Phone have labeled, visible Home actions that open their existing
shell surfaces. Phone is shown only when its service is present. The quick panel
may scroll when media and optional pins expand it; controls remain keyboard
reachable without hover.

Network opens on connection status and the network list. **Connection Details** contains
addresses, throughput, DNS, and labeled diagnostic and sharing actions. It is a live
sub-pane: status changes and QR results keep updating while it is open. **Wi-Fi Settings**
contains filtering preferences. Both pages use the same Back and Escape behavior and return
keyboard focus to the originating navigation row. A missing backend retains a readable
explanation and disables actions that cannot run.

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
3. **Many tabs still keep settings behind the Settings button.** Sound, Displays,
   Notifications, Power, Bluetooth, Network, and System use sub-panes so far. Hub sub-tabs can't be hidden individually with
   `hidden_tabs` yet, and there is no account card at the top of the sidebar.
4. About 45 raw radius and spacing numbers and about 50 literal colors remain in `src/shell`
   and `src/ui`.

## Native operation feedback and microphone tests

Standalone Settings shows ongoing connection and pairing operations in a native preference
row with a spinner and an explicit Cancel action when the backend can cancel the operation.
Completion comes from the service, rather than treating request acceptance as success.
Backend loss clears pending feedback and retains a reconnectable unavailable state.

Microphone testing is explicit: each input device has labeled Test and Stop controls and a
native level bar. Capture stops when leaving Sound, closing Settings, losing the backend,
or allowing the short client lease to expire. Show that the microphone is active and that
samples are not saved; do not start capture simply because the Sound page is visible.

Native Appearance exposes palette catalogs through standard selection controls, with explicit
Refresh and Save Current Palette actions. Do not offer an unavailable palette source or replace
native controls with a custom editor merely to reproduce shell-specific styling. Native Network
shows wired, mobile and VPN controls only when those backend capabilities are available.

Native wallpaper catalogs use ordinary toggle buttons for selectable thumbnails,
with filenames or source IDs visible below each image. Selection and application
are separate actions. Search, filters, paging and cancellation remain visible and
keyboard reachable. Local scanning and remote downloads never block the UI thread.

Native Appearance keeps palette, accent and wallpaper library actions beside the current preview
in a wrapping GTK flow layout. Palette maintenance and wallpaper sources use grouped native rows;
avoid repeating the same browser action elsewhere in the normal page. Search exposes matching
action rows as well as configuration fields, omits unrelated actions and hides illustrative previews.

GNOME Settings and the [GNOME HIG](https://developer.gnome.org/hig/index.html) are explicit
design references. Follow its [boxed-list guidance](https://developer.gnome.org/hig/patterns/containers/boxed-lists.html)
for semantic grouping, native row controls and whole-row activation when a row has one action.
Use its [navigation guidance](https://developer.gnome.org/hig/guidelines/navigation.html) when
extracting long settings groups into adaptive subpages. Preserve Helix branding and capability boundaries.

Native Displays includes an illustrative arrangement diagram with connector labels and textual
equivalents below. Until native display editing supports safe apply/revert, expose inventory and
a clearly labeled link to the working shell display controls; do not present inactive mode controls
as if they applied changes.

Online Accounts uses GOA-provided identity, branding and account-management dialogs. Group
configured accounts separately from available providers; place branded providers first and sort
names using locale-aware comparison. Keep native whole-row actions and explain unavailable
providers. Account operations remain usable without Helix configuration services; do not rebuild
the page or cancel authentication when an unrelated service changes. Never duplicate credential
fields or expose tokens in Helix snapshots or logs.
