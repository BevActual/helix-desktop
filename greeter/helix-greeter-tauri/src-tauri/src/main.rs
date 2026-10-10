//! Helix Greeter — greetd login UI (Tauri). Scaffold only: greetd IPC, appearance sync, and
//! trusted-surface framing land in follow-up work. See `greeter/HELIX.md`.

use tauri::{WebviewUrl, WebviewWindowBuilder};

const WINDOW: &str = "greeter";

fn main() {
    tauri::Builder::default()
        .setup(|app| {
            WebviewWindowBuilder::new(app, WINDOW, WebviewUrl::App("index.html".into()))
                .title("Helix")
                .fullscreen(true)
                .decorations(false)
                .build()?;
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("helix-greeter");
}
