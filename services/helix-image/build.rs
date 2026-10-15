//! `helix-image` worker. Links stable-diffusion only (its ggml must not share a crate with whisper).

use std::path::PathBuf;

fn main() {
    println!("cargo:rustc-check-cfg=cfg(helix_sd)");
    println!("cargo:rerun-if-changed=native/image_engine.c");
    println!("cargo:rerun-if-changed=build.rs");
    let root = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap()).join("../..");
    let prefix = root.join("third_party/sd-install");
    let lib = prefix.join("lib64");
    if !lib.join("libstable-diffusion.a").exists() {
        return;
    }
    cc::Build::new()
        .warnings(false)
        .include(prefix.join("include"))
        .file("native/image_engine.c")
        .compile("helix_image_shim");
    println!("cargo:rustc-cfg=helix_sd");
    println!("cargo:rustc-link-search=native={}", lib.display());
    // The vendored stable-diffusion archive is not position-independent; override rustc's -pie.
    println!("cargo:rustc-link-arg=-no-pie");
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rustc-link-arg=-Wl,--start-group");
    println!("cargo:rustc-link-lib=static=stable-diffusion");
    println!("cargo:rustc-link-lib=static=ggml");
    println!("cargo:rustc-link-lib=static=ggml-base");
    println!("cargo:rustc-link-lib=static=ggml-cpu");
    if lib.join("libggml-vulkan.a").exists() {
        println!("cargo:rustc-link-lib=static=ggml-vulkan");
        println!("cargo:rustc-link-lib=dylib=vulkan");
    }
    println!("cargo:rustc-link-arg=-Wl,--end-group");
    println!("cargo:rustc-link-lib=dylib=gomp");
    println!("cargo:rustc-link-lib=dylib=stdc++");
    println!("cargo:rustc-link-lib=dylib=dl");
}
