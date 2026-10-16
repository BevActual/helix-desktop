//! Regenerates `tests/golden/template/*` from the Rust template engine (replaces C++ `gen_golden_template.cpp`).

use std::fs;
use std::io::{self, Write};
use std::path::{Path, PathBuf};

use helix_theme::custom::{builtin_palettes, expand_fixed_palette};
use helix_theme::template::{make_theme_data, render, Options};

fn fnv1a(text: &str) -> u64 {
    let mut h: u64 = 1469598103934665603;
    for b in text.bytes() {
        h ^= u64::from(b);
        h = h.wrapping_mul(1099511628211);
    }
    h
}

fn collect_files(root: &Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    fn walk(dir: &Path, root: &Path, out: &mut Vec<PathBuf>) {
        for entry in fs::read_dir(dir).unwrap().flatten() {
            let path = entry.path();
            if entry.file_type().unwrap().is_dir() {
                walk(&path, root, out);
            } else {
                out.push(path.strip_prefix(root).unwrap().to_path_buf());
            }
        }
    }
    walk(root, root, &mut out);
    out.sort();
    out
}

fn main() -> io::Result<()> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        eprintln!(
            "usage: {} <assets/templates> <synthetic.tpl> [full-output-dir]",
            args[0]
        );
        std::process::exit(1);
    }
    let templates_dir = Path::new(&args[1]);
    let synthetic = fs::read_to_string(&args[2])?;
    let full_dir = args.get(3).map(PathBuf::from);

    let icon_theme = std::env::var("HELIX_ICON_THEME").unwrap_or_else(|_| "Adwaita".into());
    let files = collect_files(templates_dir);

    let config = serde_json::json!({
        "shell": {
            "name": "helix_shell",
            "size": 12,
            "ratio": 1.5,
            "enabled": true,
            "list": ["a", "b", "c"],
            "nested": {"k": "v"}
        }
    });

    let stdout = io::stdout();
    let mut out = stdout.lock();
    writeln!(out, "icon_theme\t{icon_theme}")?;

    for palette in builtin_palettes() {
        let generated = expand_fixed_palette(&palette.palette);
        let theme_data = make_theme_data(&generated.dark, &generated.light);
        for mode in ["dark", "light"] {
            let options = Options {
                default_mode: mode.to_string(),
                image_path: "/img/wall paper.png".into(),
                closest_color: "#112233".into(),
                config_dir: "/home/u/.config/helix".into(),
                config_file: "/home/u/.config/helix/config.toml".into(),
                config_table: Some(config.clone()),
                icon_theme: icon_theme.clone(),
            };

            let mut emit = |name: &str, text: &str, errors: u32| -> io::Result<()> {
                writeln!(
                    out,
                    "{}\t{}\t{}\t{}\t{:016x}",
                    palette.name,
                    mode,
                    name,
                    errors,
                    fnv1a(text)
                )?;
                if let Some(dir) = &full_dir {
                    fs::create_dir_all(dir)?;
                    let file_name = format!("{}_{mode}_{name}", palette.name);
                    fs::write(dir.join(file_name), text)?;
                }
                Ok(())
            };

            let synthetic_result = render(&theme_data, &options, &synthetic);
            emit(
                "synthetic.tpl",
                &synthetic_result.text,
                synthetic_result.error_count,
            )?;

            for rel in &files {
                let source = fs::read_to_string(templates_dir.join(rel))?;
                let result = render(&theme_data, &options, &source);
                emit(
                    &format!("t:{}", rel.display()),
                    &result.text,
                    result.error_count,
                )?;
            }
        }
    }
    Ok(())
}
