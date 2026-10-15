//! Optional native engines for `helix-inference`:
//! - llama.cpp glue under `native/inference/` when `third_party/llama-install` exists
//! - whisper.cpp C shim (`native/transcribe_engine.c`) for `helix-transcribe` when whisper is installed
//!
//! Image generation is the separate `helix-image` crate (its ggml must not share a link with whisper).

use std::path::{Path, PathBuf};

fn main() {
    println!("cargo:rustc-check-cfg=cfg(helix_llama)");
    println!("cargo:rustc-check-cfg=cfg(helix_whisper)");
    println!("cargo:rerun-if-changed=native/llama_shim.cpp");
    println!("cargo:rerun-if-changed=native/transcribe_engine.c");
    println!("cargo:rerun-if-changed=native/inference/llama_backend.cpp");
    println!("cargo:rerun-if-changed=native/inference/schema_grammar.cpp");
    println!("cargo:rerun-if-changed=build.rs");

    let root = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap()).join("../..");
    link_llama(&root);
    link_whisper(&root);
}

fn link_llama(root: &Path) {
    let prefix = root.join("third_party/llama-install");
    let lib = prefix.join("lib64");
    if !lib.join("libllama.so").exists() {
        return;
    }
    let native = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap()).join("native");
    cc::Build::new()
        .cpp(true)
        .std("c++23")
        .warnings(false)
        .include(&native)
        .include(prefix.join("include"))
        .file(native.join("llama_shim.cpp"))
        .file(native.join("inference/llama_backend.cpp"))
        .file(native.join("inference/schema_grammar.cpp"))
        .compile("helix_llama");
    println!("cargo:rustc-cfg=helix_llama");
    println!("cargo:rustc-link-search=native={}", lib.display());
    let lib = lib.canonicalize().unwrap_or(lib);
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib.display());
    println!("cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN/../lib64/helix");
    for name in ["llama", "mtmd", "ggml", "ggml-base"] {
        println!("cargo:rustc-link-lib=dylib={name}");
    }
    println!("cargo:rustc-link-lib=dylib=stdc++");
}

fn link_whisper(root: &Path) {
    let prefix = root.join("third_party/whisper-install");
    let lib = prefix.join("lib64");
    if !lib.join("libwhisper.a").exists() {
        return;
    }
    cc::Build::new()
        .file("native/transcribe_engine.c")
        .include(prefix.join("include"))
        .warnings(false)
        .compile("helix_whisper_shim");
    println!("cargo:rustc-cfg=helix_whisper");
    println!("cargo:rustc-link-search=native={}", lib.display());
    println!("cargo:rustc-link-arg=-Wl,--start-group");
    println!("cargo:rustc-link-lib=static=whisper");
    println!("cargo:rustc-link-lib=static=ggml");
    println!("cargo:rustc-link-lib=static=ggml-base");
    println!("cargo:rustc-link-lib=static=ggml-cpu");
    println!("cargo:rustc-link-arg=-Wl,--end-group");
    println!("cargo:rustc-link-lib=dylib=gomp");
    println!("cargo:rustc-link-lib=dylib=stdc++");
    println!("cargo:rustc-link-lib=dylib=m");
    println!("cargo:rustc-link-lib=dylib=pthread");
    println!("cargo:rustc-link-lib=dylib=dl");
}
