# helix-shell-tauri

The Rust/Tauri shell. It is a thin face over `helix-shell-core` (panel, assistant and surface state machines) and
`helix-desktop-api` (the `org.helix.Desktop` client). The C++ shell keeps running until this one is at parity. See
`docs/design/tauri-rust-shell.md`.

**One surface.** A single layer-shell window covers the output and hosts the bar and every panel in one page, so there is one
WebKit web process however many panels exist. `ShellSurface` (in `helix-shell-core`) decides the layer, keyboard mode and input
region for each state; `src-tauri/src/layer.rs` applies it. Rust decides what is open and what the assistant shows; `dist/shell.js`
only draws it. A window's capabilities (`capabilities/default.json`) must name the window label, or its events silently stop.

The surface is sized before layer-shell init and the input region is re-applied on every resize. Do not set
`WEBKIT_DISABLE_DMABUF_RENDERER`, `WEBKIT_DISABLE_COMPOSITING_MODE`, or software GL.

```bash
cd rust && cargo build --release -p helix-shell-tauri
tools/nested/wl-pointer/build.sh                                   # once: the virtual pointer used by the pointer test
tools/nested/run.sh m2 tools/nested/tauri-shell.sh                 # memory and screenshots (HELIX_NESTED_SCRIPT=tools/nested/tauri-m2.sh)
HELIX_NESTED_SCRIPT=tools/nested/single-surface-pointer-test.sh tools/nested/run.sh ptr tools/nested/tauri-shell.sh
```

The shell starts `helix-assistant` for assistant requests. It does not call Agent1 and it does not talk to the model.
