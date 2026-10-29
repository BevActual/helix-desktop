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
    // Parts of the page are React (`../web`), built into `dist/react` when the frontend workspace is installed; without it the
    // hand-written drawing in chat.js is used.
    let root = Path::new("../../..");
    let skip = std::env::var_os("HELIX_SKIP_FRONTEND").is_some_and(|v| !v.is_empty());
    if !skip && root.join("node_modules").is_dir() {
        match std::process::Command::new("npm")
            .args(["run", "build", "--workspace", "helix-mobius-web", "--silent"])
            .current_dir(root)
            .status()
        {
            Ok(status) if status.success() => {}
            Ok(status) if std::env::var_os("HELIX_ALLOW_STALE_FRONTEND").is_some() => println!("cargo:warning=Mobius UI did not build ({status}); the older drawing is used"),
            // A failed build leaves the previous pages in place, and a warning is easy to miss (cargo -q hides it): stop instead.
            Ok(status) => panic!("Mobius UI did not build ({status}); run `npm run build` in the workspace to see why, or set HELIX_ALLOW_STALE_FRONTEND=1"),
            Err(error) => println!("cargo:warning=npm not available ({error}); the hand-written drawing is used"),
        }
    }
    tauri_build::build()
}
