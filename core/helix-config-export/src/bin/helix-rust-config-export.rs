//! CLI for the in-process Rust config exporter (fixture regeneration and smoke checks).

use std::process::ExitCode;

use helix_config::settings::ConfigPaths;
use helix_config_export::config_export::rust_export_toml;

fn main() -> ExitCode {
    if let Some(arg) = std::env::args().nth(1) {
        if arg == "--help" || arg == "-h" {
            println!("Usage: helix-rust-config-export");
            println!("Print schema-normalized effective config (Rust config_export) on stdout.");
            return ExitCode::SUCCESS;
        }
        eprintln!("error: unknown argument: {arg}");
        return ExitCode::from(1);
    }

    let paths = ConfigPaths::from_env();
    match rust_export_toml(&paths) {
        Some(root) => {
            println!("{}", toml::to_string(&root).expect("serialize export"));
            ExitCode::SUCCESS
        }
        None => {
            eprintln!("config export failed: merged config is not patchable for Rust export");
            ExitCode::from(1)
        }
    }
}
