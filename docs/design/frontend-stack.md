# Frontend stack

**Direction (decided 2026-10-10): Helix apps are Tauri 2 applications with a React + TypeScript + Vite frontend.** Rust owns state and
authority; React draws views and sends back the names those views listed.

This page is the source of truth for how app UIs are built. When code and this page disagree, fix one of them in the same change.

## Scope

| Surface | Stack | Why |
|---|---|---|
| Apps under `apps/` (Settings, Mobius, Software, System Monitor, new apps) | **Tauri + React + TypeScript + Vite** | Run on demand; polish and maintainability matter most |
| Session shell: bar, panels, dock, island, OSD (`shell/helix-shell-tauri`) | **Tauri + React + TypeScript + Vite**, moved in slices (see Status) | Decided 2026-10-11 after measuring: the framework costs 0–3 MB per process in the nested harness, inside run-to-run noise |
| Approval dialog (confirmer) | Tauri + hand-written HTML/CSS/JS, no build step | Security-critical: small enough to audit, no framework between the person and the decision |
| Greeter | Tauri + React; lock screen stays native `swaylock` for now | Decided with the owner: the greeter is a React page over a Rust greetd client that offers only listed accounts and sessions; the lock cannot depend on a web page until a session-lock host exists |
| Helpers (`services/`) | Rust only | No UI |

A new app starts as React. The confirmer does not become React without changing this page first.

**How the shell moves.** The shell page (`shell/helix-shell-tauri/dist`) hosts the bar and every panel in one document, with geometry
reporting (input regions, glass rectangles, test layout) tied to element ids. It moves slice by slice: a React island renders into an
existing container (its id and geometry stay), `shell.js` keeps the mode, the regions and the glass until the last slice, and each slice
keeps the hand-written drawing as the fallback for a build without node. When every slice is React, `shell.js` becomes the thin React
entry point and the fallbacks are deleted.

## Layout

| Path | What |
|---|---|
| `package.json`, `package-lock.json`, `tsconfig.json`, `eslint.config.js` (repo root) | One npm workspace for every React app and the shared package: one `node_modules`, one lockfile |
| `core/shared-ui/react/` | `@helix/ui`: typed Tauri bridge (`invoke`, `listen`) and the shared components (`Row`, `RowGroups`, `Switch`, `Slider`, `Segments`), on the shared CSS tokens (`core/shared-ui/style.css`) |
| `core/shared-ui/react/bindings/` | TypeScript types generated from Rust view structs (`tools/gen-bindings.sh`; `ts-rs`, cargo feature `ts`). Committed, never edited by hand |
| `apps/<app>/web/` | The app's React source. Builds one script, `apps/<app>/dist/react/helix-react.js` (not committed) |
| `apps/<app>/dist/` | The page the Tauri window loads: `index.html`, shared CSS/icons, and any hand-written fallback |
| `apps/<app>/src-tauri/` | The Rust host (commands, state, D-Bus) |

`build.rs` of an app runs `npm run build --workspace <app>-web` when `node_modules` exists. Without node (or with
`HELIX_SKIP_FRONTEND=1`) the build still succeeds, cargo warns, and the hand-written page is used.

## Rules

1. **Rust owns state and authority.** React holds only what the person is typing and what is on screen. Every mutation is a typed Tauri
   command; the model in `helix-shell-core` decides whether it is legal.
2. **Text is a text node.** Text from a model, an app, a notification or the system is only ever a React child. Never
   `dangerouslySetInnerHTML`, never `innerHTML`. `react/no-danger` and a ban on `innerHTML` assignment are lint errors. Markdown reaches the
   page as blocks Rust has already parsed (`helix_shell_core::markdown`).
3. **Strict CSP, no remote content, no `eval`.** Each Tauri window's capability lists only the commands and window actions it needs.
4. **Types come from Rust.** A view the page receives derives `ts_rs::TS` (behind the `ts` feature); a command's arguments are named in
   the page by the types the view listed. Hand-mirrored types live in `apps/<app>/web/src/types.ts` with a comment naming the Rust struct.
5. **Tests find controls by `data-test`.** Components keep (or add) stable `data-test` names; the nested tests click by name with a real
   pointer. Do not rename them in a visual change.
6. **One look.** Components use the shared CSS tokens and classes (`setting-row`, `rows-card`, `switch`, `segments`, …). No literal
   colours or sizes in app code; no second design system. The visual language is `helix-hig.md`.
7. **No framework state library by default.** Component state and Tauri events are enough; add one (Zustand) for a page that needs it,
   and say why in the PR.

## Commands

```sh
npm install                          # once, at the repo root (Node 22+)
npm run typecheck && npm run lint    # also run by tools/check-all.sh
tools/gen-bindings.sh                # after changing a view struct that derives TS
cargo build --release -p helix-settings   # builds the React bundle first when node_modules exists
cd apps/helix-settings/web && npx vite build   # just the bundle
```

## Adding a page or an app

**A page in an existing app:** add `src/pages/<Name>Page.tsx`, register it in `src/main.tsx` (`custom` for a component, `specs` for a page
that is only rows), give the Rust host the commands it needs, and keep the `data-test` names. Settings pages mount into the hand-written
sidebar through `window.HelixReact.mountPage(name, host, options)`.

**A new app:** `apps/<app>/{src-tauri,web,dist}`; add `apps/<app>/web` to the root `workspaces`; copy `build.rs` from Settings (shared
CSS/icons plus the optional frontend build); add the app to `tools/check-all.sh` and `ui-suite.sh` with a nested test; list it in
`product-structure.md`.

## Status

| Surface | State |
|---|---|
| **Settings** | **All pages React**. Sidebar, search and page chrome still in `settings.js`; each page's hand-written drawing is the fallback and is deleted after a release has run on the React page |
| **Shell: notifications, launcher, Today, control center, quick insert, Mobius dropdown, island, OSD content and tray (icons and the application's menu), dock icons** | **React islands**. Control center = the whole panel body (clock and battery, tiles, brightness, sound and microphone sliders, media, system buttons and power sheet, the Sound, Internet and Bluetooth detail pages, the Wi-Fi password sheet). The Mobius dropdown (Talk, Act, Agents) draws Rust's conversation, pending request and audit views and sends back only a typed request, the one confirm, or an id it was given; the bar's agent glyph stays in `shell.js`. Quick insert (the Caps Lock palette) draws Rust's rows and sends back only the index and generation of the one chosen. `shell.js` keeps the bar's own glyphs, the theme attribute, the mode and the regions, and leaves the panel DOM to React when its bundle is present. Improvements that came with the move: launcher shows a "Suggested" label, highlights the match, keeps the selected row in view, Home/End, hover selection, an Enter hint; Today picks a day, and Left/Right/T move through months |
| **Greeter** | **React** (`greeter/helix-greeter-tauri/web`); greetd protocol and the account/session lists are Rust, see `greeter-tauri.md` |
| **Lock screen** | `swaylock` themed with the shared tokens through `helix-panel lock`. A Helix-drawn lock needs an ext-session-lock host for a webview (`gtk-session-lock`, not installed here); until then it stays native so the lock can never depend on a web page |
| Shell: bar (its glyph slots, workspaces, clock; the dock's magnify lens, hide timer, tooltip and menu also stay in `shell.js` because the input regions and glass are measured from them) | Hand-written; moving in that order (the dock and bar last: the magnify and region maths are the most geometry-coupled) |
| Mobius app (`helix-mobius`) | **Moving in slices** (`apps/helix-mobius/web`, built into `dist/react/helix-mobius.js`; `chat.js` keeps each hand-written part as the no-node fallback). Done: the **message thread** (markdown blocks as elements, streaming, find highlighting, edit sheet, copy/act/quote/fork/regenerate), the **persona rail and the rename, project, persona and model sheets** (`mobius-dialogs` test), **chat list** (search with Rust's full-text hits, project filter, open/delete) and the **Models & routing page** (draft editing, per-task routing, model grids, LAN/cloud probes, secret store; `tools/nested/ui-suite.sh models`). Next: the composer (slash menu, attachments, queue, prompt history); `chat.js` keeps the find bar, scroll and header |
| **Software** | **React** (`apps/helix-software/web`); hand-written page kept as the no-node fallback |
| **System Monitor** | **React** (`apps/helix-system-monitor/web`); hand-written page kept as the no-node fallback |

## Measured cost (release builds, same nested tests)

Settings (whole app React vs hand-written):

| | hand-written | React |
|---|---|---|
| App memory, Appearance page (PSS) | 147.0 MB | 150.2 MB |
| App memory, Dock page (PSS) | 149.3 MB | 153.2 MB |
| Script shipped | none | 250 KB (77 KB gzip) |
| Time to show a page | no measurable difference | |

About 3–4 MB on a 150 MB app: the WebKit baseline dominates.

Shell (React islands for notifications, launcher, Today, the control center, quick insert, the Mobius dropdown and the island/OSD content and tray vs hand-written; total PSS of the shell, its WebKit
processes and the service; two runs each, release builds):

| | hand-written | React islands |
|---|---|---|
| Idle | 160.4 / 161.9 MB | 167.2 / 166.5 MB |
| Shade open | 162.3 / 163.8 MB | 168.9 / 168.7 MB |

Bundle: 243 KB (75 KB gzip). The cost is about **+5 MB** (the React runtime, four roots, and the control center's state), above the run-to-run
noise of about 1.5 MB. It grows slowly per slice now because the runtime is the fixed part. Re-measure with `tools/nested/measure-settings-test.sh` and
`tools/nested/measure-shell-test.sh` as slices land and when the bundle or dependency set grows; the budget table in `tauri-rust-shell.md`
is the limit.

## Known costs and open questions

* **Packaging.** Fedora and Nix packaging needs a locked, vendored, offline `node_modules`. Not done; the build degrades to the
  hand-written pages without it, which is the safety net until it is.
* **Supply chain.** npm's dependency tree is new surface for a Rust-only project. `package-lock.json` is committed; `npm ci` in CI; the
  dependency set is deliberately small (react, react-dom, vite, typescript, eslint plugins).
* **Component library.** Plain React over our own CSS today. Radix UI (or React Aria) is the plan for menus, dialogs and popovers when a
  page needs them; styling stays ours.
* **Settings shell.** The sidebar and search are still hand-written; they move when the last fallback is deleted.

## History

The pilot (Settings → Dock, 2026-10-10) and the measurements behind this decision are in git history on `docs/design/frontend-stack.md`.
Alternatives weighed (vanilla JS, Preact, Svelte, Solid, Lit, Dioxus, Slint, GTK4/libadwaita, Qt) and why they were not chosen: React has
the largest ecosystem for the accessible components and long lists a chat or settings app needs, the best tooling, and the most familiar
code for contributors and AI-assisted work; the always-on shell keeps the no-framework approach where footprint and auditability matter.
