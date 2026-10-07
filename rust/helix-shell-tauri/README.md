# helix-shell-tauri

The Rust/Tauri shell. It is a thin face over `helix-shell-core` (panel and assistant state) and `helix-desktop-api`
(the `org.helix.Desktop` client). The C++ shell keeps running until this one is at parity. See
`docs/design/tauri-rust-shell.md`.

Layer surfaces are created hidden, given a size request, then initialized with gtk-layer-shell. Do not set
`WEBKIT_DISABLE_DMABUF_RENDERER`, `WEBKIT_DISABLE_COMPOSITING_MODE`, or software GL.

```bash
cd rust && cargo build --release -p helix-shell-tauri
tools/nested/run.sh tauri-m2 tools/nested/tauri-shell.sh
```

The shell starts `helix-assistant` for assistant requests. It does not call Agent1 and it does not talk to the model.
