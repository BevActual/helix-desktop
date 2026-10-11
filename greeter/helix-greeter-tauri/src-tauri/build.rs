use std::path::Path;

/// Design tokens and icons live in `core/shared-ui/` and are copied into this app's `dist` at build.
fn main() {
    let shared = Path::new("../../../core/shared-ui");
    let own = Path::new("../dist");
    for file in ["style.css", "icons.js"] {
        let from = shared.join(file);
        println!("cargo:rerun-if-changed={}", from.display());
        std::fs::copy(&from, own.join(file))
            .unwrap_or_else(|error| panic!("cannot copy shared {file}: {error}"));
    }
    println!("cargo:rerun-if-changed=../dist");
    println!("cargo:rerun-if-changed=../web/src");
    println!("cargo:rerun-if-env-changed=HELIX_SKIP_FRONTEND");
    // The login card is React (`../web`), built into `dist/react` when the frontend workspace is installed (`npm install` at the repo root).
    let root = Path::new("../../..");
    let skip = std::env::var_os("HELIX_SKIP_FRONTEND").is_some_and(|v| !v.is_empty());
    if !skip && root.join("node_modules").is_dir() {
        match std::process::Command::new("npm")
            .args(["run", "build", "--workspace", "helix-greeter-web", "--silent"])
            .current_dir(root)
            .status()
        {
            Ok(status) if status.success() => {}
            Ok(status) => println!("cargo:warning=greeter UI did not build ({status}); the greeter shows a plain message"),
            Err(error) => println!("cargo:warning=npm not available ({error}); the greeter shows a plain message"),
        }
    }
    tauri_build::build()
}
