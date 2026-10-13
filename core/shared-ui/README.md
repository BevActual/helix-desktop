# Shared UI tokens

`style.css` and `icons.js` are the single design-token source for Helix Tauri apps
(shell, settings, chat, software, system-monitor). Each app's `build.rs` copies them
into its own `dist/` at build time. Edit here, not under an app's `dist/`.
