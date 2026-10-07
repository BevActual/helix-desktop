fn main() {
    // Dist is not a Rust input; without this, confirmer.html/js edits never rebuild the binary.
    println!("cargo:rerun-if-changed=../dist");
    tauri_build::build()
}
