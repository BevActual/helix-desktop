# Helix visual language for applications

**Direction (decided):** *if Apple made a Linux desktop and its applications.* Calm, precise, content first; the system is the star of the
craft, not the individual app. This page is the rule for every app under `apps/` and for the shell's panels. It **supersedes** the
"prefer standard GTK/libadwaita controls" contract in `helix-hig.md` for applications (the HIG's materials, tiers, color roles, menus and
accessibility sections still apply and are restated here where the apps need them).

Related: `frontend-stack.md` (how apps are built), `helix-hig.md` (shell chrome, bar, Control Center).

## 1. The three ideas

1. **Clarity.** Type is legible at a glance, icons are precise, hierarchy is obvious. If two things look equally important, one of them is
   wrong.
2. **Deference.** The interface frames the content and gets out of the way: neutral surfaces, one accent, almost no borders. Color is
   reserved for selection, state and data.
3. **Depth.** Layers have meaning. Solid is *content*; translucent material is *chrome that floats above or beside it*; shadows and motion
   say where something came from. Never decoration.

Corollaries: **consistency beats cleverness** (the same control looks and behaves the same in every app), **direct manipulation** (act on the
thing you see), **forgiveness** (undo, confirm only what is destructive, never lose typed text), and **delight lives at the edges**
(a spring on a sheet, a considered empty state, nothing flashing).

## 2. Window anatomy

```
┌───────────────┬──────────────────────────────────────────────┐
│ ● (close)     │  Title               [toolbar actions]  🔍    │  unified titlebar + toolbar, 52 px
│               ├──────────────────────────────────────────────┤
│  Sidebar      │                                              │
│  (material)   │   Content (solid)                            │
│  240 px       │   one scrolling column, max 720 px of text   │
│               │                                              │
└───────────────┴──────────────────────────────────────────────┘
```

- **Unified titlebar.** No separate titlebar strip: the window control (close, then minimise/zoom when the compositor offers them), the
  page title and the toolbar share one 52 px row. The row is a drag region. It is *transparent over content* and gains a hairline and a
  material when content scrolls beneath it.
- **Sidebar** is a **material** (translucent where the compositor blurs, a solid tint where it does not), 240 px, collapsible. Selection is
  a filled accent pill when the window is active and a neutral fill when it is not. Section headers are small caps-free, secondary text.
- **Content is solid** (`--content`), always. Text sits on opaque surfaces so contrast never depends on the wallpaper.
- **Inactive windows recede:** sidebar selection goes neutral, toolbar icons dim to 55%, the accent disappears from controls. This is a
  core part of the feel and costs one `:root[data-window="inactive"]` rule set by the host on focus change.
- Default window 1040×720, minimum 720×480. Below 600 px the sidebar becomes a drawer over the content.
- One window per app; secondary things are **sheets** (modal, attached to the title, slide down) or **popovers** (anchored, arrowed).
  No nested windows.

## 3. Materials

| Name | Where | Look |
|---|---|---|
| `content` | window body, cards, lists | opaque `--content` (`#1e1e22` dark, `#ffffff` light) |
| `sidebar` | window sidebar | translucent 62% + blur 30 px + saturate 1.4; solid `--sidebar-solid` fallback |
| `titlebar` | toolbar row | clear at rest; `--content` at 80% + blur 20 px when scrolled |
| `menu` / `popover` | menus, popovers, tooltips | `--glass` + blur, 12 px radius, 0.5 px inner hairline, soft layered shadow |
| `sheet` | modal sheets | `--content` over a 30% dim; 18 px radius at the bottom corners |
| `grouped` | settings-style groups | `--fill-1` well, 12 px radius, 0.5 px separators inset 16 px from the leading edge |

Blur is a **progressive enhancement**: `data-blur="1"` on `:root` (already set by the shell when the compositor honours blur rules). Without
it every material is its solid fallback; nothing may depend on blur for legibility.

Edges: a 0.5 px inner hairline (`--hairline`) on every raised surface, in both themes. Shadows are two layers (a tight contact shadow and a
wide soft one), never a single hard drop shadow.

## 4. Color

- **One accent, user-chosen, global.** Default `#0a84ff` (blue). The user may pick another from a fixed set (blue, purple, pink, red,
  orange, yellow, green, graphite) in Appearance; apps read `--accent`. **Apps never set their own accent.**
- Neutrals are an eight-step label/fill ladder (`--label`, `--label-2`, `--label-3`; `--fill-1…3`; `--sep`, `--hairline`).
- Semantic only: `--ok`, `--warn`, `--danger` for state and destructive actions. Never for decoration.
- Light and dark are designed *as equals*; neither is "the inverted one". Contrast ≥ 4.5:1 for text, ≥ 3:1 for large text and icons.
  High Contrast replaces fills with outlines and drops materials.
- Content color (an app icon tile, a chart series) is allowed and expected: it is data, not chrome.

## 5. Typography

One family: **Inter** (OFL; a packaging dependency: `rsms-inter-fonts` on Fedora, `inter` on Nix), with **Inter Display** for titles
(optical size) and Cantarell / Noto Sans as the fallback. Feature set `cv11`, `ss03`; tabular numerals where numbers align. It stands in for
the system face, so the scale below is the whole contract, not the font name.

| Role | Size / line | Weight | Use |
|---|---|---|---|
| Large title | 26 / 32 | 700 | onboarding, empty states, one per window at most |
| Title | 17 / 22 | 650 | page title in the toolbar |
| Headline | 13 / 18 | 650 | group header, row title when it leads |
| Body | 13 / 18 | 400–500 | everything |
| Callout | 12 / 16 | 400 | secondary line under a row |
| Caption | 11 / 14 | 500 | badges, footnotes, section labels |

Rules: sentence case everywhere; numbers, times and sizes are **tabular**; letter-spacing −0.006 em on body, −0.02 em on titles; never more
than three type sizes visible in one view; truncate with an ellipsis, never wrap a title.

## 6. Shape, space, size

- **Radii** (concentric: inner = outer − padding): window 12, sheet/popover 12–18, card/group 12, control 8–10, pill 999, icon tile 22 %.
- **Spacing grid 4 px.** Page padding 24; group gap 20; row padding 12/16; inline gap 8.
- **Controls:** regular 30 px high, large 36 px (touch / primary), small 24 px. Hit target ≥ 28 px pointer, ≥ 44 px touch (large density).
- **Rows:** 44 px with a subtitle, 36 px compact. Density is a token (`--density: roomy | compact`); Settings and Software are roomy,
  Monitor and the Mobius sidebar are compact.
- **Icons:** our set (`core/shared-ui/icons.js`), 18 px in rows and toolbars, 24 px in empty states; 1.7 px stroke, round caps, optically
  centered, weight matched to the text beside them. Sidebar items use an icon at 16 px. App/section tiles are a colored rounded square with a
  white glyph (Settings-style). No emoji as icons.

## 7. Components (one of each)

| Component | Rule |
|---|---|
| **Button** | Default (neutral fill), **Primary** (accent fill, one per view, bound to Enter), **Destructive** (red text, red fill only inside the confirming sheet), **Plain** (text only, toolbars). Pill radius only for chips. |
| **Segmented control** | 2–5 mutually exclusive views or modes; sliding thumb; replaces tabs. |
| **Switch** | 38×22, immediate effect, no Save button. |
| **Text field / search** | 30 px, recessed fill, 2 px accent focus ring; search has a leading magnifier and a clear button. |
| **Popup / menu** | Native-feeling: checkmark for the current value, ⌃ shortcuts right-aligned, separators; opens on the trigger, no scrolling menus over 12 items (use a search). |
| **Row / group** | Inset grouped lists. Row = icon tile? · title · (callout) · trailing value/control · chevron when it navigates. |
| **Card** | Only for *content that is an object* (an app, a model, a chat). Not for grouping settings. |
| **Sheet** | Modal for a focused task: title, content, **Cancel (Esc)** left, **Primary (Enter)** right. |
| **Alert** | Centered, 2–3 actions, destructive action named as a verb ("Delete Chat"), Cancel is the default for destructive alerts. |
| **Banner** | Inline, under the toolbar: info / warn / error with an action. Never a toast. |
| **Empty state** | Icon (24 px, `--label-3`), a sentence saying what goes here, one action that fixes it. |
| **Meter / progress** | 4–6 px bar, accent; indeterminate = a slow sweep, never a spinner wheel for more than a moment. |
| **Badge / chip** | Caption weight, 18 px high, fill tint of the semantic color. |

## 8. Motion

- **One curve, one family of durations:** `cubic-bezier(.22,.9,.3,1)` (a soft spring); 120 ms for press and hover, 220 ms for reveals, 320 ms
  for sheets and layout changes.
- Sheets slide from the title bar, popovers scale from their anchor (from 96 %), the sidebar collapses by width, lists reorder with a 220 ms
  move. Things come *from* somewhere.
- Press feedback is a 3 % scale and a fill step, not a ripple. Reduced Motion: cross-fades or nothing (`--dur: 1ms`, already global).
- Never animate text reflow; never loop attention-seeking animation.

## 9. Behavior

- **Keyboard first, always:** Tab order = visual order; `Ctrl+,` Settings, `Ctrl+F` find, `Ctrl+N` new, `Ctrl+W` close, `Ctrl+Z` undo, `Esc`
  dismisses, `Enter` activates the primary; arrow keys move in lists; type-to-select in lists. Shortcuts are shown in menus.
- **Selection model:** click selects, double-click opens, right-click opens the context menu, long-press (touch) the same.
- **Undo over confirm:** reversible actions apply immediately and offer Undo for 8 s; only destructive or irreversible actions confirm.
- **Never lose input:** drafts survive navigation; errors keep what was typed.
- **Live updates don't move things under the pointer:** lists keep scroll and selection; new items appear without shifting the row being
  read.
- **Authority is visible:** anything that acts on the system (install, remove, grant, change routing) shows what it will do in words, names
  the object, and waits for Allow. (This is the shell's trust rule; apps follow it.)

## 10. Accessibility and platform

- High Contrast, Reduced Motion, and Increase Text Size (UI scale) apply everywhere through tokens; nothing hard-codes px for type.
- Every control has an accessible name and a `data-test` name; focus is always visible; no information by color alone.
- Layout works at 360, 768 and 1280 px widths; touch targets grow with density `roomy`.
- Apps run without the shell or helix-niri; desktop-only pages are omitted when their capability is absent.

## 11. What we do not do

- No per-app branding, accent or custom window chrome.
- No gradient fills, glow, neon, or skeuomorphism; glass signals "floats", nothing else.
- No hamburger menus, toast stacks, floating action buttons, or dashboard card grids.
- No emoji as UI, no all-caps headings (caption labels excepted), no text under 11 px.
- No second version of a component without a reason written here first.

## 12. Gaps against this document (the work list)

1. ~~Font~~ Done: tokens name Inter / Inter Display with fallbacks; packaging lists the font package (it is installed on the dev machine).
2. ~~Tokens~~ Done in `style.css` (`--content`, `--sidebar`, `--density`, accent set, radii, spring); remove the old per-app classes as
   each app moves to the kit.
3. ~~Primitives~~ Done in `core/shared-ui/react/kit.tsx` (Software is the first app on it; Settings, Monitor, Mobius next). **Primitives in `@helix/ui`:** `AppFrame` (titlebar + sidebar + content + inactive state), `Toolbar`, `Group`/`Row`, `Segmented`,
   `Switch`, `Field`/`SearchField`, `Sheet`, `Alert`, `Banner`, `EmptyState`, `Meter`, `Chip`, `IconTile`. Apps compose these; `style.css`
   classes become an implementation detail.
4. ~~Window state~~ Done: `AppFrame` sets `data-window` from window focus.
5. **Audit.** Settings, Software, Monitor and Mobius against sections 2–9; fix density, titlebar and empty states first.
6. **Appearance.** Accent choice and density setting in Settings → Appearance, stored once and read by every app.

## 13. Patterns proven in Software

- **Large title** at the top of the content; the toolbar title appears only when it has scrolled away (`AppFrame largeTitle`).
- **Shelves**: a titled horizontal rail of tiles (icon, name, one line, one control) with *See All*.
- **Hero**: one featured object, tinted from its own icon, never a banner ad.
- **Ring control**: a running job is the arc of a circle with a stop square; it replaces the button in place.
- **Detail page**: hero (icon, developer, summary, one primary), screenshots rail, facts strip, prose, then metadata chips.
- Real artwork is always preferred; a lettered tile is the fallback, never the design.
