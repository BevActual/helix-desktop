# Single-surface experiment (reference only, not built)

One layer-shell surface anchored to all four edges hosts the bar and every panel in a single page, so there is one WebKit
web process however many panels are open. `exclusive_zone` is the bar height; the GDK input shape limits clicks to the bar
while closed and to the whole surface while a panel is open (a scrim dismisses on outside click); opening a panel raises
the layer to Overlay and takes exclusive keyboard, closing returns to Top with no keyboard.

`main.rs` and `index.html` were built as a throwaway Tauri app (same `Cargo.toml` and `tauri.conf.json` as
`../../helix-shell-tauri/src-tauri`, window label `shell`). Measured with `tools/nested/run.sh` (see `docs/design/tauri-rust-shell.md`).
