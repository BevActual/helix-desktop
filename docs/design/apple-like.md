# Apple-like interfaces

The direction (2026-10-23): design Helix applications with the feel of macOS and iOS: calm, restrained, native-looking. This is a reference for *feel*,
not a rulebook to copy: where something is better for Helix we do it (see `helix-hig.md`), and Apple's own pages are inputs we read and apply in our
own words. Apple's Human Interface Guidelines are at developer.apple.com/design/human-interface-guidelines (a JavaScript page; read it in a
browser). Their design resources are at developer.apple.com/design/resources.

## Assets: reference, not material

Apple's fonts (SF Pro, New York), SF Symbols and the UI kits are provided under Apple's licence for designing and building apps *for Apple's
platforms*. Helix is not one, so we do not ship, embed or trace them. We look at them for proportions, spacing, weights and motion, and draw our own:
the system font stack and the icon set in `core/shared-ui/icons.js`. Anything we add to the kit is ours.

## What "Apple-like" means here

* **Hierarchy before chrome.** A sidebar for places, a large title for where you are, grouped inset rows for settings. Controls recede; content leads.
* **Values live on the right of a row.** A choice is a **pop-up button** (a chevron, a menu of a few options with the current one ticked), two to four
  exclusive options a **segmented control**, on/off a **switch**. Never a native `<select>`, a bare browser text field, or a wall of form inputs.
* **One thing at a time.** Adding or editing happens in a **sheet** over the window (Escape cancels, the primary action is a button), destructive
  actions ask in a small alert sheet with the destructive button named for what it does ("Remove", "Delete"), never "OK".
* **Progressive disclosure.** The common case is one row; advanced detail is behind a row or a sheet, not on the page by default.
* **Honest status.** Chips and plain sentences say what is true ("Key saved", "Your network", "No graphics card is being used"); no jargon where a
  sentence will do; an empty state says what to do next.
* **Direct and reversible.** Changes apply as they are made; a mistake is undone by choosing again, and what cannot be undone is confirmed first.
* **Motion that explains.** Menus and sheets ease in over ~120 ms; nothing bounces, nothing blocks.
* **Respect the person's settings.** Light or dark, accent, interface scale, corner roundness and glass follow Settings (`shape-settings.json`).
* **Keyboard and assistive tech work.** Roles and labels on every control, arrow keys in menus, visible focus.

## Where it lives

The shared kit is `core/shared-ui/react/kit.tsx` with its styles in `core/shared-ui/style.css` (AppFrame, Sidebar, Group, Row, Chip, Banner,
Sheet, Segmented, Switch, and **Popup**, added 2026-10-23). A new piece an app needs goes into the kit so every app gets it. Settings, Mobius and
the older pages are brought toward this as they are touched; Helix Models (`apps/helix-models`) is the first built entirely on it.

## Checklist for a new screen

1. Is it a place (sidebar), a setting (inset row), or a task (sheet)?
2. Is every choice a popup, segmented control or switch?
3. Is anything shown that the common case does not need?
4. Does a destructive action name itself and confirm?
5. Does it look right in light, dark, at 0.8 and 1.5 scale, and square or round?
