# helix-tauri-spike

Throwaway experiment, not part of Helix. A Tauri 2 app with two windows turned into layer-shell surfaces with
gtk-layer-shell: a top bar (reserves space) and an overlay assistant panel (exclusive keyboard) that drives
`helix-assistant --json` the same way the native panel does. Used to measure what a web-based shell costs.

    cd src-tauri && cargo build --release
    ./target/release/helix-tauri-spike     # run inside a Wayland session with layer-shell
