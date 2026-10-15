//! Builds the llama.cpp engine glue (C++, `src/inference/llama_backend.cpp`) when the engine is installed under
//! `third_party/llama-install` (see docs/design/agent-and-inference.md). Without it the service builds with only the
//! scripted engine, as the C++ service did.

use std::path::PathBuf;

fn main() {
    println!("cargo:rustc-check-cfg=cfg(helix_llama)");
    println!("cargo:rerun-if-changed=native/llama_shim.cpp");
    println!("cargo:rerun-if-changed=build.rs");
    let root = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap()).join("../..");
    let prefix = root.join("third_party/llama-install");
    let lib = prefix.join("lib64");
    if !lib.join("libllama.so").exists() {
        return;
    }
    let src = root.join("src");
    for file in ["inference/llama_backend.cpp", "inference/schema_grammar.cpp", "inference/llama_backend.h", "inference/backend.h"] {
        println!("cargo:rerun-if-changed={}", src.join(file).display());
    }
    cc::Build::new()
        .cpp(true)
        .std("c++23")
        .warnings(false)
        .include(&src)
        .include(prefix.join("include"))
        .file("native/llama_shim.cpp")
        .file(src.join("inference/llama_backend.cpp"))
        .file(src.join("inference/schema_grammar.cpp"))
        .compile("helix_llama");
    println!("cargo:rustc-cfg=helix_llama");
    println!("cargo:rustc-link-search=native={}", lib.display());
    let lib = lib.canonicalize().unwrap_or(lib);
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib.display()); // development: the engine is built into third_party/
    println!("cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN/../lib64/helix"); // installed: the private copy beside bin/
    for name in ["llama", "mtmd", "ggml", "ggml-base"] {
        println!("cargo:rustc-link-lib=dylib={name}");
    }
    println!("cargo:rustc-link-lib=dylib=stdc++");
}
