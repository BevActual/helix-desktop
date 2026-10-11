# Frontend stack

Decision (2026-10-10): **the shell, bar, panels, greeter and approval dialog stay hand-written** (small, always running, security-critical).
**Apps under `apps/` move to React + TypeScript + Vite inside Tauri**, one page at a time, over the same Rust commands.

## Layout

| Path | What |
|---|---|
| `package.json` (repo root) | npm workspace for the React apps and the shared package; one `node_modules`, one lockfile |
| `core/shared-ui/react/` | `@helix/ui`: typed Tauri bridge, the settings row family (`Row`, `Switch`, `Slider`, `Segments`, `RowGroups`) on the existing CSS tokens |
| `core/shared-ui/react/bindings/` | TypeScript types generated from the Rust view structs (`tools/gen-bindings.sh`, `ts-rs`, feature `ts`) |
| `apps/<app>/web/` | the app's React source; builds one script into `apps/<app>/dist/react/` (not committed) |
| `apps/<app>/dist/` | the hand-written page; it loads `react/helix-react.js` when present and keeps its own version as the fallback |

## Rules

* Rust owns state and authority; React only draws views and sends the names a view listed.
* Text from a model, an app or the system is only ever a React child. `react/no-danger` and a ban on `innerHTML` are lint errors.
* Controls keep their `data-test` names so the nested tests are unchanged.
* The build never requires node: with no `node_modules` (or `HELIX_SKIP_FRONTEND=1`) the hand-written pages are used and cargo warns.

## Commands

```sh
npm install                 # once, at the repo root
npm run typecheck && npm run lint
tools/gen-bindings.sh       # after changing a view struct that derives TS
cargo build -p helix-settings   # builds the React bundle first when node_modules exists
```

## Pilot result: Settings > Dock

Same nested tests (`settings`, `settings-dock`) pass against the React page.

| | hand-written | React |
|---|---|---|
| App memory, Appearance page (PSS) | 147.0 MB | 148.7 MB |
| App memory, Dock page (PSS) | 149.4 MB | 152.4 MB |
| Script shipped | none extra | 225 KB (70 KB gzip) |
| Binary size | 8.42 MB | 8.48 MB |

The cost is about 3 MB on a 150 MB app (the WebKit baseline dominates) and no measurable change in time to show the page.

## Ported so far

**All Settings pages are React**: Dock, Power & lock, Displays, Appearance, Accessibility (one generic `RowsPage` driven by a spec per page),
and About, Keyboard, Wallpaper (with Wallhaven search), Network (password, enterprise, VPN), Bluetooth (pairing), Privacy & agents (own
components). The sidebar, search and the page chrome stay in `settings.js`, and each page keeps its hand-written drawing as the fallback for a
build without node. The nested `settings`, `settings-net` and `settings-dock` tests and the Wallhaven test pass unchanged.

## Next

Mobius (streaming messages, edit/branch, search, attachments), Software and Monitor. Then the Settings shell itself (sidebar, search page) and
the deletion of the hand-written page drawing once a release has run on the React pages.
