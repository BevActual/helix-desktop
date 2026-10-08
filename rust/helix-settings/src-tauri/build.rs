use std::path::Path;

/// The design tokens and the icon set are shared with the shell: one copy, in the shell's `dist`. They are copied next to this app's own
/// pages at build time (the copies are not committed), so the two apps cannot drift apart.
fn main() {
    let shared = Path::new("../../helix-shell-tauri/dist");
    let own = Path::new("../dist");
    for file in ["style.css", "icons.js"] {
        println!("cargo:rerun-if-changed={}", shared.join(file).display());
        std::fs::copy(shared.join(file), own.join(file))
            .unwrap_or_else(|error| panic!("cannot copy shared {file}: {error}"));
    }
    println!("cargo:rerun-if-changed=../dist");
    tauri_build::build()
}
